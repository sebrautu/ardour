/*
    Copyright (C) 2009 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include "ardour/session.h"

#include "canvas/debug.h"

#include "time_axis_view.h"
#include "streamview.h"
#include "editor_summary.h"
#include "gui_thread.h"
#include "editor.h"
#include "region_view.h"
#include "rgb_macros.h"
#include "keyboard.h"
#include "editor_routes.h"
#include "editor_cursors.h"
#include "mouse_cursors.h"
#include "route_time_axis.h"
#include "ui_config.h"

using namespace std;
using namespace ARDOUR;
using Gtkmm2ext::Keyboard;

/** Construct an EditorSummary.
 *  @param e Editor to represent.
 */
EditorSummary::EditorSummary (Editor* e)
	: EditorComponent (e),
	  _start (0),
	  _end (1),
	  _overhang_fraction (0.02),
	  _x_scale (1),
	  _track_height (16),
	  _last_playhead (-1),
	  _begin_dragging (false),
	  _move_dragging (false),
	  _moved (false),
	  _view_rectangle_x (0, 0),
	  _view_rectangle_y (0, 0),
	  _zoom_trim_dragging (false),
	  _zoom_dragging (false),
	  _old_follow_playhead (false),
	  _image (0),
	  _background_dirty (true)
{
	CairoWidget::use_nsglview ();
	add_events (Gdk::POINTER_MOTION_MASK|Gdk::KEY_PRESS_MASK|Gdk::KEY_RELEASE_MASK|Gdk::ENTER_NOTIFY_MASK|Gdk::LEAVE_NOTIFY_MASK);
	set_flags (get_flags() | Gtk::CAN_FOCUS);

	UIConfiguration::instance().ParameterChanged.connect (sigc::mem_fun (*this, &EditorSummary::parameter_changed));
}

EditorSummary::~EditorSummary ()
{
	cairo_surface_destroy (_image);
}

void
EditorSummary::parameter_changed (string p)
{

	if (p == "color-regions-using-track-color") {
		set_background_dirty ();
	}
}

/** Handle a size allocation.
 *  @param alloc GTK allocation.
 */
void
EditorSummary::on_size_allocate (Gtk::Allocation& alloc)
{
	CairoWidget::on_size_allocate (alloc);
	set_background_dirty ();
}


/** Connect to a session.
 *  @param s Session.
 */
void
EditorSummary::set_session (Session* s)
{
	SessionHandlePtr::set_session (s);

	set_dirty ();

	/* Note: the EditorSummary already finds out about new regions from Editor::region_view_added
	 * (which attaches to StreamView::RegionViewAdded), and cut regions by the RegionPropertyChanged
	 * emitted when a cut region is added to the `cutlist' playlist.
	 */

	if (_session) {
		Region::RegionPropertyChanged.connect (region_property_connection, invalidator (*this), boost::bind (&EditorSummary::set_background_dirty, this), gui_context());
		PresentationInfo::Change.connect (route_ctrl_id_connection, invalidator (*this), boost::bind (&EditorSummary::set_background_dirty, this), gui_context());
		_editor->playhead_cursor->PositionChanged.connect (position_connection, invalidator (*this), boost::bind (&EditorSummary::playhead_position_changed, this, _1), gui_context());
		_session->StartTimeChanged.connect (_session_connections, invalidator (*this), boost::bind (&EditorSummary::set_background_dirty, this), gui_context());
		_session->EndTimeChanged.connect (_session_connections, invalidator (*this), boost::bind (&EditorSummary::set_background_dirty, this), gui_context());
		_editor->selection->RegionsChanged.connect (sigc::mem_fun(*this, &EditorSummary::set_background_dirty));
	}
	
	_leftmost = _session->current_start_frame();
	_rightmost = min (_session->nominal_frame_rate()*60*2, _session->current_start_frame() );  //always show at least 2 minutes
}

void
EditorSummary::render_background_image ()
{
	cairo_surface_destroy (_image); // passing NULL is safe
	_image = cairo_image_surface_create (CAIRO_FORMAT_RGB24, get_width (), get_height ());

	cairo_t* cr = cairo_create (_image);

	/* background (really just the dividing lines between tracks */

	cairo_set_source_rgb (cr, 0, 0, 0);
	cairo_rectangle (cr, 0, 0, get_width(), get_height());
	cairo_fill (cr);

	/* compute start and end points for the summary */

	framecnt_t const session_length = _session->current_end_frame() - _session->current_start_frame ();
	double theoretical_start = _session->current_start_frame() - session_length * _overhang_fraction;
	double theoretical_end = _session->current_end_frame();

	/* the summary should encompass the full extent of everywhere we've visited since the session was opened */
	if ( _leftmost < theoretical_start)
		theoretical_start = _leftmost;
	if ( _rightmost > theoretical_end )
		theoretical_end = _rightmost;

	/* range-check */
	_start = theoretical_start > 0 ? theoretical_start : 0;
	_end = theoretical_end + session_length * _overhang_fraction;

	/* calculate x scale */
	if (_end != _start) {
		_x_scale = static_cast<double> (get_width()) / (_end - _start);
 	} else {
		_x_scale = 1;
	}

	/* compute track height */
	int N = 0;
	for (TrackViewList::const_iterator i = _editor->track_views.begin(); i != _editor->track_views.end(); ++i) {
		if (!(*i)->hidden()) {
			++N;
		}
	}

	if (N == 0) {
		_track_height = 16;
	} else {
		_track_height = (double) get_height() / N;
	}

	/* render tracks and regions */

	double y = 0;
	for (TrackViewList::const_iterator i = _editor->track_views.begin(); i != _editor->track_views.end(); ++i) {

		if ((*i)->hidden()) {
			continue;
		}

		/* paint a non-bg colored strip to represent the track itself */

		cairo_set_source_rgb (cr, 0.2, 0.2, 0.2);
		cairo_set_line_width (cr, _track_height - 1);
		cairo_move_to (cr, 0, y + _track_height / 2);
		cairo_line_to (cr, get_width(), y + _track_height / 2);
		cairo_stroke (cr);

		StreamView* s = (*i)->view ();

		if (s) {
			cairo_set_line_width (cr, _track_height * 0.8);

			s->foreach_regionview (sigc::bind (
						       sigc::mem_fun (*this, &EditorSummary::render_region),
						       cr,
						       y + _track_height / 2
						       ));
		}

		y += _track_height;
	}

	/* start and end markers */

	cairo_set_line_width (cr, 1);
	cairo_set_source_rgb (cr, 1, 1, 0);

	const double p = (_session->current_start_frame() - _start) * _x_scale;
	cairo_move_to (cr, p, 0);
	cairo_line_to (cr, p, get_height());

	double const q = (_session->current_end_frame() - _start) * _x_scale;
	cairo_move_to (cr, q, 0);
	cairo_line_to (cr, q, get_height());
	cairo_stroke (cr);

	cairo_destroy (cr);
}

/** Render the required regions to a cairo context.
 *  @param cr Context.
 */
void
EditorSummary::render (Cairo::RefPtr<Cairo::Context> const& ctx, cairo_rectangle_t*)
{
	cairo_t* cr = ctx->cobj();

	if (_session == 0) {
		return;
	}

	/* maintain the leftmost and rightmost locations that we've ever reached */
	framecnt_t const leftmost = _editor->leftmost_sample ();
	if ( leftmost < _leftmost) {
		_leftmost = leftmost;
		_background_dirty = true;
	}
	framecnt_t const rightmost = leftmost + _editor->current_page_samples();
	if ( rightmost > _rightmost) {
		_rightmost = rightmost;
		_background_dirty = true;
	}

	//draw the background (regions, markers, etc ) if they've changed
	if (!_image || _background_dirty) {
		render_background_image ();
		_background_dirty = false;
	}

	cairo_push_group (cr);

	/* Fill with the background image */

	cairo_rectangle (cr, 0, 0, get_width(), get_height());
	cairo_set_source_surface (cr, _image, 0, 0);
	cairo_fill (cr);

	/* Render the view rectangle.  If there is an editor visual pending, don't update
	 * the view rectangle now --- wait until the expose event that we'll get after
	 * the visual change.  This prevents a flicker.
	 */

	if (_editor->pending_visual_change.idle_handler_id < 0) {
		get_editor (&_view_rectangle_x, &_view_rectangle_y);
	}

	int32_t width = _view_rectangle_x.second - _view_rectangle_x.first;
	std::min(8, width);
	int32_t height = _view_rectangle_y.second - _view_rectangle_y.first;
	cairo_rectangle (cr, _view_rectangle_x.first, 0, width, get_height ());
	cairo_set_source_rgba (cr, 1, 1, 1, 0.15);
	cairo_fill (cr);

	/* horiz zoom */
	cairo_rectangle (cr, _view_rectangle_x.first, 0, width, get_height ());
	cairo_set_line_width (cr, 1);
	cairo_set_source_rgba (cr, 1, 1, 1, 0.9);
	cairo_stroke (cr);

	/* Playhead */

	cairo_set_line_width (cr, 1);
	/* XXX: colour should be set from configuration file */
	cairo_set_source_rgba (cr, 1, 0, 0, 1);

	const double ph= playhead_frame_to_position (_editor->playhead_cursor->current_frame());
	cairo_move_to (cr, ph, 0);
	cairo_line_to (cr, ph, get_height());
	cairo_stroke (cr);
	cairo_pop_group_to_source (cr);
	cairo_paint (cr);
	_last_playhead = ph;

}

/** Render a region for the summary.
 *  @param r Region view.
 *  @param cr Cairo context.
 *  @param y y coordinate to render at.
 */
void
EditorSummary::render_region (RegionView* r, cairo_t* cr, double y) const
{
	uint32_t const c = r->get_fill_color ();
	cairo_set_source_rgb (cr, UINT_RGBA_R (c) / 255.0, UINT_RGBA_G (c) / 255.0, UINT_RGBA_B (c) / 255.0);

	if (r->region()->position() > _start) {
		cairo_move_to (cr, (r->region()->position() - _start) * _x_scale, y);
	} else {
		cairo_move_to (cr, 0, y);
	}

	if ((r->region()->position() + r->region()->length()) > _start) {
		cairo_line_to (cr, ((r->region()->position() - _start + r->region()->length())) * _x_scale, y);
	} else {
		cairo_line_to (cr, 0, y);
	}

	cairo_stroke (cr);
}

void
EditorSummary::set_background_dirty ()
{
	if (!_background_dirty) {
		_background_dirty = true;
		set_dirty ();
	}
}

/** Set the summary so that just the overlays (viewbox, playhead etc.) will be re-rendered */
void
EditorSummary::set_overlays_dirty ()
{
	ENSURE_GUI_THREAD (*this, &EditorSummary::set_overlays_dirty);
	queue_draw ();
}

/** Set the summary so that just the overlays (viewbox, playhead etc.) in a given area will be re-rendered */
void
EditorSummary::set_overlays_dirty (int x, int y, int w, int h)
{
	ENSURE_GUI_THREAD (*this, &EditorSummary::set_overlays_dirty);
	queue_draw_area (x, y, w, h);
}


/** Handle a size request.
 *  @param req GTK requisition
 */
void
EditorSummary::on_size_request (Gtk::Requisition *req)
{
	/* The left/right buttons will determine our height */
	req->width = -1;
	req->height = -1;
}


void
EditorSummary::centre_on_click (GdkEventButton* ev)
{
	pair<double, double> xr;
	get_editor (&xr);

	double const w = xr.second - xr.first;
	double ex = ev->x - w / 2;
	if (ex < 0) {
		ex = 0;
	} else if ((ex + w) > get_width()) {
		ex = get_width() - w;
	}

	set_editor (ex);
}

bool
EditorSummary::on_enter_notify_event (GdkEventCrossing*)
{
	grab_focus ();
	Keyboard::magic_widget_grab_focus ();
	return false;
}

bool
EditorSummary::on_leave_notify_event (GdkEventCrossing*)
{
	/* there are no inferior/child windows, so any leave event means that
	   we're gone.
	*/
	Keyboard::magic_widget_drop_focus ();
	return false;
}

bool
EditorSummary::on_key_press_event (GdkEventKey* key)
{
	gint x, y;
	GtkAccelKey set_playhead_accel;
	if (gtk_accel_map_lookup_entry ("<Actions>/Editor/set-playhead", &set_playhead_accel)) {
		if (key->keyval == set_playhead_accel.accel_key && (int) key->state == set_playhead_accel.accel_mods) {
			if (_session) {
				get_pointer (x, y);
				_session->request_locate (_start + (framepos_t) x / _x_scale, _session->transport_rolling());
				return true;
			}
		}
	}

	return false;
}

bool
EditorSummary::on_key_release_event (GdkEventKey* key)
{

	GtkAccelKey set_playhead_accel;
	if (gtk_accel_map_lookup_entry ("<Actions>/Editor/set-playhead", &set_playhead_accel)) {
		if (key->keyval == set_playhead_accel.accel_key && (int) key->state == set_playhead_accel.accel_mods) {
			return true;
		}
	}
	return false;
}

/** Handle a button press.
 *  @param ev GTK event.
 */
bool
EditorSummary::on_button_press_event (GdkEventButton* ev)
{
	_old_follow_playhead = _editor->follow_playhead ();

	if (ev->button != 1) {
		return true;
	}

	pair<double, double> xr;
	get_editor (&xr);

	_start_editor_x = xr;
	_start_mouse_x = ev->x;
	_start_mouse_y = ev->y;
	_start_position = get_position (ev->x, ev->y);

	if (_start_position != INSIDE && _start_position != TO_LEFT_OR_RIGHT) {

		/* start a zoom_trim drag */

		_zoom_trim_position = get_position (ev->x, ev->y);
		_zoom_trim_dragging = true;
		_editor->_dragging_playhead = true;
		_editor->set_follow_playhead (false);

		if (suspending_editor_updates ()) {
			get_editor (&_pending_editor_x, &_pending_editor_y);
			_pending_editor_changed = false;
		}

	} else if (Keyboard::modifier_state_equals (ev->state, Keyboard::SecondaryModifier)) {

		/* secondary-modifier-click: locate playhead */
		if (_session) {
			_session->request_locate (ev->x / _x_scale + _start);
		}

	} else if (Keyboard::modifier_state_equals (ev->state, Keyboard::TertiaryModifier)) {

		centre_on_click (ev);

	} else {

		/* start a move or zoom drag */
		/* won't know which one until the mouse moves */
		_begin_dragging = true;
	}

	return true;
}

/** @return true if we are currently suspending updates to the editor's viewport,
 *  which we do if configured to do so, and if in a drag of some kind.
 */
bool
EditorSummary::suspending_editor_updates () const
{
	return (!UIConfiguration::instance().get_update_editor_during_summary_drag () && (_zoom_dragging || _zoom_trim_dragging || _move_dragging));
}

/** Fill in x and y with the editor's current viewable area in summary coordinates */
void
EditorSummary::get_editor (pair<double, double>* x, pair<double, double>* y) const
{
	assert (x);
	if (suspending_editor_updates ()) {

		/* We are dragging, and configured not to update the editor window during drags,
		 * so just return where the editor will be when the drag finishes.
		*/

		*x = _pending_editor_x;
		if (y) {
			*y = _pending_editor_y;
		}
		return;
	}

	/* Otherwise query the editor for its actual position */

	x->first = (_editor->leftmost_sample () - _start) * _x_scale;
	x->second = x->first + _editor->current_page_samples() * _x_scale;

	if (y) {
		y->first = editor_y_to_summary (_editor->vertical_adjustment.get_value ());
		y->second = editor_y_to_summary (_editor->vertical_adjustment.get_value () + _editor->visible_canvas_height() - _editor->get_trackview_group()->canvas_origin().y);
	}
}

/** Get an expression of the position of a point with respect to the view rectangle */
EditorSummary::Position
EditorSummary::get_position (double x, double y) const
{
	/* how close the mouse has to be to the edge of the view rectangle to be considered `on it',
	   in pixels */

	int x_edge_size = (_view_rectangle_x.second - _view_rectangle_x.first) / 4;
	x_edge_size = min (x_edge_size, 8);
	x_edge_size = max (x_edge_size, 1);

	bool const near_left = (std::abs (x - _view_rectangle_x.first) < x_edge_size);
	bool const near_right = (std::abs (x - _view_rectangle_x.second) < x_edge_size);
	bool const within_x = _view_rectangle_x.first < x && x < _view_rectangle_x.second;

	if (near_left) {
		return LEFT;
	} else if (near_right) {
		return RIGHT;
	} else if (within_x) {
		return INSIDE;
	} else {
		return TO_LEFT_OR_RIGHT;
	}
}

void
EditorSummary::set_cursor (Position p)
{
	switch (p) {
	case LEFT:
		get_window()->set_cursor (*_editor->_cursors->resize_left);
		break;
	case RIGHT:
		get_window()->set_cursor (*_editor->_cursors->resize_right);
		break;
	case INSIDE:
		get_window()->set_cursor (*_editor->_cursors->move);
		break;
	case TO_LEFT_OR_RIGHT:
		get_window()->set_cursor (*_editor->_cursors->move);
		break;
	default:
		assert (0);
		get_window()->set_cursor ();
		break;
	}
}

void
EditorSummary::summary_zoom_step ( int steps /* positive steps to zoom "out" , negative steps to zoom "in" */  )
{
	pair<double, double> xn;

	get_editor (&xn);
//	{
//		xn.first = (_editor->leftmost_sample () - _start) * _x_scale;
//		xn.second = xn.first + _editor->current_page_samples() * _x_scale;
//	}

	xn.first -= steps;
	xn.second += steps;

	set_overlays_dirty ();
	set_editor_x (xn);
}


bool
EditorSummary::on_motion_notify_event (GdkEventMotion* ev)
{
	pair<double, double> xr = _start_editor_x;
	double x = _start_editor_x.first;

	if (_move_dragging) {

		_moved = true;

		assert (_start_position == INSIDE || _start_position == TO_LEFT_OR_RIGHT);
		x += ev->x - _start_mouse_x;

		if (x < 0) {
			x = 0;
		}

		set_editor (x);

	} else if (_zoom_dragging) {

		//ToDo: refactor into summary_zoom_in/out(
		//ToDo:  protect the case where the editor position is small, and results in offsetting the position

		double const dy = ev->y - _zoom_last_y;
		
		summary_zoom_step( dy );

		_zoom_last_y = ev->y;
			
	} else if (_zoom_trim_dragging) {

		double const dx = ev->x - _start_mouse_x;

		if (_zoom_trim_position == LEFT) {
			xr.first += dx;
		} else if (_zoom_trim_position == RIGHT) {
			xr.second += dx;
		} else {
			assert (0);
			xr.first = -1; /* do not change */
		}

		set_overlays_dirty ();
		set_cursor (_zoom_trim_position);
		set_editor (xr);

	} else if (_begin_dragging) {

		double const dx = ev->x - _start_mouse_x;
		double const dy = ev->y - _start_mouse_y;

		if ( fabs(dx) > fabs(dy) ) {
			
			/* initiate a move drag */

			/* get the editor's state in case we are suspending updates */
			get_editor (&_pending_editor_x, &_pending_editor_y);
			_pending_editor_changed = false;

			_move_dragging = true;
			_moved = false;
			_editor->_dragging_playhead = true;
			_editor->set_follow_playhead (false);

			get_window()->set_cursor (*_editor->_cursors->expand_left_right);

			_begin_dragging = false;
		
		} else if ( fabs(dy) > fabs(dx) ) {
		
			/* initiate a zoom drag */

			/* get the editor's state in case we are suspending updates */
			get_editor (&_pending_editor_x, &_pending_editor_y);
			_pending_editor_changed = false;

			//_zoom_position = get_position (ev->x, ev->y);
			_zoom_dragging = true;
			_zoom_last_y = ev->y;
			_editor->_dragging_playhead = true;
			_editor->set_follow_playhead (false);

			get_window()->set_cursor (*_editor->_cursors->expand_up_down);

			_begin_dragging = false;
		}
		
	} else {
		set_cursor ( get_position(ev->x, ev->y) );
	}

	return true;
}

bool
EditorSummary::on_button_release_event (GdkEventButton*)
{
	bool const was_suspended = suspending_editor_updates ();

	_begin_dragging = false;
	_move_dragging = false;
	_zoom_trim_dragging = false;
	_zoom_dragging = false;
	_editor->_dragging_playhead = false;
	_editor->set_follow_playhead (_old_follow_playhead, false);

	if (was_suspended && _pending_editor_changed) {
		set_editor (_pending_editor_x);
	}

	return true;
}

bool
EditorSummary::on_scroll_event (GdkEventScroll* ev)
{
	/* mouse wheel */
	pair<double, double> xr;
	get_editor (&xr);
	double x = xr.first;

	switch (ev->direction) {
		case GDK_SCROLL_UP: {
			
			summary_zoom_step( -4 );
		
			return true;
		} break;
		
		case GDK_SCROLL_DOWN: {
			
			summary_zoom_step( 4 );
		
			return true;
		} break;
		
		case GDK_SCROLL_LEFT:
			if (Keyboard::modifier_state_equals (ev->state, Keyboard::ScrollZoomHorizontalModifier)) {
				_editor->temporal_zoom_step (false);
			} else if (Keyboard::modifier_state_contains (ev->state, Keyboard::SecondaryModifier)) {
				x -= 64;
			} else if (Keyboard::modifier_state_contains (ev->state, Keyboard::TertiaryModifier)) {
				x -= 1;
			} else {
				_editor->scroll_left_half_page ();
				return true;
			}
			break;
		case GDK_SCROLL_RIGHT:
			if (Keyboard::modifier_state_equals (ev->state, Keyboard::ScrollZoomHorizontalModifier)) {
				_editor->temporal_zoom_step (true);
			} else if (Keyboard::modifier_state_contains (ev->state, Keyboard::SecondaryModifier)) {
				x += 64;
			} else if (Keyboard::modifier_state_contains (ev->state, Keyboard::TertiaryModifier)) {
				x += 1;
			} else {
				_editor->scroll_right_half_page ();
				return true;
			}
			break;
		default:
			break;
	}

	set_editor (x);
	return true;
}

/** Set the editor to display a x range with the left at a given position
 *  and a y range with the top at a given position.
 *  x and y parameters are specified in summary coordinates.
 *  Zoom is not changed in either direction.
 */
void
EditorSummary::set_editor (double const x)
{
	if (_editor->pending_visual_change.idle_handler_id >= 0 && _editor->pending_visual_change.being_handled == true) {

		/* As a side-effect, the Editor's visual change idle handler processes
		   pending GTK events.  Hence this motion notify handler can be called
		   in the middle of a visual change idle handler, and if this happens,
		   the queue_visual_change calls below modify the variables that the
		   idle handler is working with.  This causes problems.  Hence this
		   check.  It ensures that we won't modify the pending visual change
		   while a visual change idle handler is in progress.  It's not perfect,
		   as it also means that we won't change these variables if an idle handler
		   is merely pending but not executing.  But c'est la vie.
		*/

		return;
	}

	set_editor_x (x);
}

/** Set the editor to display a given x range and a y range with the top at a given position.
 *  The editor's x zoom is adjusted if necessary, but the y zoom is not changed.
 *  x and y parameters are specified in summary coordinates.
 */
void
EditorSummary::set_editor (pair<double,double> const x)
{
	if (_editor->pending_visual_change.idle_handler_id >= 0) {
		/* see comment in other set_editor () */
		return;
	}

	if (x.first >= 0) {
		set_editor_x (x);
	}
}

/** Set the left of the x range visible in the editor.
 *  Caller should have checked that Editor::pending_visual_change.idle_handler_id is < 0
 *  @param x new x left position in summary coordinates.
 */
void
EditorSummary::set_editor_x (double x)
{
	if (x < 0) {
		x = 0;
	}

	if (suspending_editor_updates ()) {
		double const w = _pending_editor_x.second - _pending_editor_x.first;
		_pending_editor_x.first = x;
		_pending_editor_x.second = x + w;
		_pending_editor_changed = true;
		set_dirty ();
	} else {
		_editor->reset_x_origin (x / _x_scale + _start);
	}
}

/** Set the x range visible in the editor.
 *  Caller should have checked that Editor::pending_visual_change.idle_handler_id is < 0
 *  @param x new x range in summary coordinates.
 */
void
EditorSummary::set_editor_x (pair<double, double> x)
{
	if (x.first < 0) {
		x.first = 0;
	}

	if (x.second < 0) {
		x.second = x.first + 1;
	}

	if (suspending_editor_updates ()) {
		_pending_editor_x = x;
		_pending_editor_changed = true;
		set_dirty ();
	} else {
		_editor->reset_x_origin (x.first / _x_scale + _start);

		double const nx = (
			((x.second - x.first) / _x_scale) /
			_editor->sample_to_pixel (_editor->current_page_samples())
			);

		if (nx != _editor->get_current_zoom ()) {
			_editor->reset_zoom (nx);
		}
	}
}

void
EditorSummary::playhead_position_changed (framepos_t p)
{
	int const o = int (_last_playhead);
	int const n = int (playhead_frame_to_position (p));
	if (_session && o != n) {
		int a = max(2, min (o, n));
		int b = max (o, n);
		set_overlays_dirty (a - 2, 0, b + 2, get_height ());
	}
}

double
EditorSummary::editor_y_to_summary (double y) const
{
	double sy = 0;
	for (TrackViewList::const_iterator i = _editor->track_views.begin (); i != _editor->track_views.end(); ++i) {

		if ((*i)->hidden()) {
			continue;
		}

		double const h = (*i)->effective_height ();
		if (y < h) {
			/* in this track */
			return sy + y * _track_height / h;
		}

		sy += _track_height;
		y -= h;
	}

	return sy;
}

void
EditorSummary::routes_added (list<RouteTimeAxisView*> const & r)
{
	for (list<RouteTimeAxisView*>::const_iterator i = r.begin(); i != r.end(); ++i) {
		/* Connect to the relevant signal for the route so that we know when its colour has changed */
		(*i)->route()->presentation_info().PropertyChanged.connect (*this, invalidator (*this), boost::bind (&EditorSummary::route_gui_changed, this, _1), gui_context ());
		boost::shared_ptr<Track> tr = boost::dynamic_pointer_cast<Track> ((*i)->route ());
		if (tr) {
			tr->PlaylistChanged.connect (*this, invalidator (*this), boost::bind (&EditorSummary::set_background_dirty, this), gui_context ());
		}
	}

	set_background_dirty ();
}

void
EditorSummary::route_gui_changed (PBD::PropertyChange const& what_changed)
{
	if (what_changed.contains (Properties::color)) {
		set_background_dirty ();
	}
}

double
EditorSummary::playhead_frame_to_position (framepos_t t) const
{
	return (t - _start) * _x_scale;
}

framepos_t
EditorSummary::position_to_playhead_frame_to_position (double pos) const
{
	return _start  + (pos * _x_scale);
}
