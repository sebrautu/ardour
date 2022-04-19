/*
 * Copyright (C) 2022 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _libardour_io_plug_h_
#define _libardour_io_plug_h_

#include <boost/shared_ptr.hpp>

#include "pbd/timing.h"
#include "pbd/g_atomic_compat.h"

#include "ardour/ardour.h"
#include "ardour/automation_control.h"
#include "ardour/buffer_set.h"
#include "ardour/plugin.h"
#include "ardour/session_object.h"
#include "ardour/plug_insert_base.h"

namespace ARDOUR {

class IO;
class ReadOnlyControl;

class LIBARDOUR_API IOPlug : public SessionObject, public PlugInsertBase
{
public:
	IOPlug (Session&, boost::shared_ptr<Plugin> = boost::shared_ptr<Plugin>(), bool pre = true);
	virtual ~IOPlug ();

	bool set_name (std::string const&);

	XMLNode& get_state (void) const;
	int set_state (const XMLNode&, int version);

	void run (samplepos_t, pframes_t);
	int set_block_size (pframes_t);
	bool ensure_io ();

	bool is_pre () const { return _pre; }

	uint32_t get_count () const { return 1; }
	boost::shared_ptr<Plugin> plugin (uint32_t num = 0) const { return _plugin; }
	PluginType type () const { return _plugin->get_info()->type; }

	UIElements ui_elements () const;

	bool write_immediate_event (Evoral::EventType event_type, size_t size, const uint8_t* buf);
	bool load_preset (Plugin::PresetRecord);

	boost::shared_ptr<ReadOnlyControl> control_output (uint32_t) const;

	bool reset_parameters_to_default () { return false;}
	bool can_reset_all_parameters () { return false; }

	virtual bool provides_stats () const { return true; }
	virtual bool get_stats (PBD::microseconds_t&, PBD::microseconds_t&, double&, double&) const;
	virtual void clear_stats ();

	boost::shared_ptr<Evoral::Control> control_factory(const Evoral::Parameter& id);

	boost::shared_ptr<IO> input () const { return _input; }
	boost::shared_ptr<IO> output () const { return _output; }

protected:
	std::string describe_parameter (Evoral::Parameter);

	/** A control that manipulates a plugin parameter (control port). */
	struct PluginControl : public AutomationControl
	{
		PluginControl (IOPlug*                    p,
		               Evoral::Parameter const&   param,
		               ParameterDescriptor const& desc);

		double get_value () const;
		void catch_up_with_external_value (double val);
		XMLNode& get_state() const;
		std::string get_user_string() const;
	private:
		void actually_set_value (double val, PBD::Controllable::GroupControlDisposition group_override);
		IOPlug* _iop;
	};

	/** A control that manipulates a plugin property (message). */
	struct PluginPropertyControl : public AutomationControl
	{
		PluginPropertyControl (IOPlug*                    p,
		                       Evoral::Parameter const&   param,
		                       ParameterDescriptor const& desc);

		double get_value () const;
		XMLNode& get_state() const;
	private:
		void actually_set_value (double value, PBD::Controllable::GroupControlDisposition);
		IOPlug* _iop;
		Variant _value;
	};

private:
	/* disallow copy construction */
	IOPlug (IOPlug const&);

	std::string ensure_io_name (std::string) const;
	void create_parameters ();
	void parameter_changed_externally (uint32_t, float);

	void setup ();

	ChanCount _n_in;
	ChanCount _n_out;
	PluginPtr _plugin;
	bool      _pre;

	typedef std::map<uint32_t, boost::shared_ptr<ReadOnlyControl> >CtrlOutMap;
	CtrlOutMap _control_outputs;

	BufferSet             _bufs;
	boost::shared_ptr<IO> _input;
	boost::shared_ptr<IO> _output;

	PBD::TimingStats  _timing_stats;
	GATOMIC_QUAL gint _stat_reset;
};

}
#endif

