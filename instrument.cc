/*
  LV2 Instrument Example Plugin
  Copyright 2011-2012 David Robillard <d@drobilla.net>
  Copyright 2011 Gabriel M. Beddingfield <gabriel@teuton.org>
  Copyright 2011 James Morris <jwm.art.net@gmail.com>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifndef __cplusplus
#    include <stdbool.h>
#endif

#include <ladspam-0/synth.h>
#include <ladspam.pb.h>
#include <fstream>
#include <vector>
#include <boost/shared_ptr.hpp>
#include <stdint.h>


#include "lv2/lv2plug.in/ns/ext/atom/forge.h"
#include "lv2/lv2plug.in/ns/ext/atom/util.h"
#include "lv2/lv2plug.in/ns/ext/log/log.h"
#include "lv2/lv2plug.in/ns/ext/log/logger.h"
#include "lv2/lv2plug.in/ns/ext/midi/midi.h"
#include "lv2/lv2plug.in/ns/ext/patch/patch.h"
#include "lv2/lv2plug.in/ns/ext/state/state.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"
#include "lv2/lv2plug.in/ns/ext/worker/worker.h"
#include "lv2/lv2plug.in/ns/lv2core/lv2.h"

#include "./uris.h"

enum {
	INSTRUMENT_CONTROL    = 0,
	INSTRUMENT_NOTIFY     = 1,
	INSTRUMENT_AUDIO_IN1  = 2,
	INSTRUMENT_AUDIO_IN2  = 3,
	INSTRUMENT_AUDIO_OUT1 = 4,
	INSTRUMENT_AUDIO_OUT2 = 5
};

ladspam::synth_ptr build_synth(const ladspam_pb::Synth& synth_pb, unsigned sample_rate, unsigned control_period)
{
	ladspam::synth_ptr the_synth(new ladspam::synth(sample_rate, control_period));
	
	for (unsigned plugin_index = 0; plugin_index < synth_pb.plugins_size(); ++plugin_index)
	{
		ladspam_pb::Plugin plugin_pb = synth_pb.plugins(plugin_index);
		
		the_synth->append_plugin
		(
			the_synth->find_plugin_library(plugin_pb.label()), 
			plugin_pb.label()
		);
		
		for (unsigned value_index = 0; value_index < plugin_pb.values_size(); ++value_index)
		{
			ladspam_pb::Value value = plugin_pb.values(value_index);
			
			the_synth->set_port_value(plugin_index, value.port_index(), value.value());
		}
	}
	
	for (unsigned connection_index = 0; connection_index < synth_pb.connections_size(); ++connection_index)
	{
		ladspam_pb::Connection connection_pb = synth_pb.connections(connection_index);
		
		the_synth->connect
		(
			connection_pb.source_index(),
			connection_pb.source_port_index(),
			connection_pb.sink_index(),
			connection_pb.sink_port_index()
		);
	}
	
	//expose_ports(synth_pb, the_synth);
	
	return the_synth;
}

struct voice
{
	float m_gate;
	unsigned m_note;
	unsigned m_on_velocity;
	unsigned m_off_velocity;
	float m_note_frequency;
	unsigned m_start_frame;
	std::vector<ladspam::synth::buffer_ptr> m_port_buffers;
	std::vector<ladspam::synth::buffer *> m_port_buffers_raw;
	
	voice(unsigned control_period) :
		m_gate(0.0),
		m_note(0),
		m_on_velocity(0),
		m_off_velocity(0),
		m_note_frequency(0),
		m_start_frame(0)
	{
		{
			ladspam::synth::buffer_ptr buffer(new std::vector<float>());
			buffer->resize(control_period);
			m_port_buffers.push_back(buffer);
			m_port_buffers_raw.push_back(buffer.get());
		}

		{
			ladspam::synth::buffer_ptr buffer(new std::vector<float>());
			buffer->resize(control_period);
			m_port_buffers.push_back(buffer);
			m_port_buffers_raw.push_back(buffer.get());
		}

		{
			ladspam::synth::buffer_ptr buffer(new std::vector<float>());
			buffer->resize(control_period);
			m_port_buffers.push_back(buffer);
			m_port_buffers_raw.push_back(buffer.get());
		}

		{
			ladspam::synth::buffer_ptr buffer(new std::vector<float>());
			buffer->resize(control_period);
			m_port_buffers.push_back(buffer);
			m_port_buffers_raw.push_back(buffer.get());
		}

		{
			ladspam::synth::buffer_ptr buffer(new std::vector<float>());
			buffer->resize(control_period);
			m_port_buffers.push_back(buffer);
			m_port_buffers_raw.push_back(buffer.get());
		}
	}
};

struct MInstrument {
	ladspam::synth_ptr synth;
	std::vector<voice> m_voices;
	std::string path;
};

typedef struct {
	// Features
	LV2_URID_Map*        map;
	LV2_Worker_Schedule* schedule;
	LV2_Log_Log*         log;

	// Forge for creating atoms
	LV2_Atom_Forge forge;

	// Logger convenience API
	LV2_Log_Logger logger;

	MInstrument *instrument;

	// Ports
	const LV2_Atom_Sequence* control_port;
	LV2_Atom_Sequence*       notify_port;
	float*                   input_port1;
	float*                   input_port2;
	float*                   output_port1;
	float*                   output_port2;

	// Forge frame for notify port (for writing worker replies)
	LV2_Atom_Forge_Frame notify_frame;

	// URIs
	InstrumentURIs uris;

	// Current position in run()
	uint32_t frame_offset;

	unsigned long samplerate;
} Instrument;

/**
   An atom-like message used internally to apply/free samples.

   This is only used internally to communicate with the worker, it is never
   sent to the outside world via a port since it is not POD.  It is convenient
   to use an Atom header so actual atoms can be easily sent through the same
   ringbuffer.
*/
typedef struct {
	LV2_Atom atom;
	MInstrument *instrument;
} InstrumentMessage;

/**
   Load a new sample and return it.

   Since this is of course not a real-time safe action, this is called in the
   worker thread only.  The sample is loaded and returned only, plugin state is
   not modified.
*/
static MInstrument*
load_instrument(Instrument* self, const char* path)
{
	// std::cout << "Loading instrument: " << path << std::endl;
	
	const size_t path_len  = strlen(path);

	lv2_log_trace(&self->logger, "Loading instrument %s\n", path);
	
	ladspam_pb::Instrument instrument_pb;
	std::ifstream input_file(path, std::ios::in | std::ios::binary);
	
	if (false == input_file.good())
	{
		std::cout << "Failed to open input stream" << std::endl;
		return 0;
	}
		
	
	if (false == instrument_pb.ParseFromIstream(&input_file))
	{
		std::cout << "Failed to parse instrument definition file" << std::endl;
		return 0;
	}
	
	MInstrument* instrument  = new MInstrument;

	try {
		ladspam::synth_ptr synth = build_synth(instrument_pb.synth(), self->samplerate, 8);
		lv2_log_trace(&self->logger, "Succeeded to load instrument\n");

		instrument->synth = synth;
		instrument->path  = path;
	} catch (std::exception &e) {
		lv2_log_trace(&self->logger, "Failed to load instrument\n");
		return 0;
	}
	
	return instrument;
}

static void
free_instrument(Instrument* self, MInstrument *instrument)
{
	if (instrument) {
		lv2_log_trace(&self->logger, "Freeing %s\n", instrument->path.c_str());
		delete instrument;
	}
}

/**
   Do work in a non-realtime thread.

   This is called for every piece of work scheduled in the audio thread using
   self->schedule->schedule_work().  A reply can be sent back to the audio
   thread using the provided respond function.
*/
static LV2_Worker_Status
work(LV2_Handle                  instance,
     LV2_Worker_Respond_Function respond,
     LV2_Worker_Respond_Handle   handle,
     uint32_t                    size,
     const void*                 data)
{
	lv2_log_trace(&((Instrument*)instance)->logger, "Loading instrument - work \n");

	Instrument*        self = (Instrument*)instance;
	const LV2_Atom* atom = (const LV2_Atom*)data;
	if (atom->type == self->uris.freeInstrument) {
		const InstrumentMessage* msg = (const InstrumentMessage*)data;
		free_instrument(self, msg->instrument);
	} else {
		// Handle set message (load sample).
		const LV2_Atom_Object* obj = (const LV2_Atom_Object*)data;

		// Get file path from message
		const LV2_Atom* file_path = read_set_file(&self->uris, obj);
		if (!file_path) {
			return LV2_WORKER_ERR_UNKNOWN;
		}

		// Load sample.
		MInstrument* instrument = (MInstrument*)load_instrument(self, (const char*)LV2_ATOM_BODY_CONST(file_path));
		if (instrument) {
			// Loaded sample, send it to run() to be applied.
			respond(handle, sizeof(instrument), &instrument);
		}
	}

	return LV2_WORKER_SUCCESS;
}

/**
   Handle a response from work() in the audio thread.

   When running normally, this will be called by the host after run().  When
   freewheeling, this will be called immediately at the point the work was
   scheduled.
*/
static LV2_Worker_Status
work_response(LV2_Handle  instance,
              uint32_t    size,
              const void* data)
{
	Instrument* self = (Instrument*)instance;

	InstrumentMessage msg = { { sizeof(MInstrument*), self->uris.freeInstrument },
	                      self->instrument };

	// Send a message to the worker to free the current sample
	self->schedule->schedule_work(self->schedule->handle, sizeof(msg), &msg);

	// Install the new sample
	self->instrument = *(MInstrument*const*)data;

	// Send a notification that we're using a new sample.
	lv2_atom_forge_frame_time(&self->forge, self->frame_offset);
	write_set_file(&self->forge, &self->uris,
	               self->instrument->path.c_str(),
	               self->instrument->path.length());

	return LV2_WORKER_SUCCESS;
}

static void
connect_port(LV2_Handle instance,
             uint32_t   port,
             void*      data)
{
	Instrument* self = (Instrument*)instance;
	switch (port) {
	case INSTRUMENT_CONTROL:
		self->control_port = (const LV2_Atom_Sequence*)data;
		break;
	case INSTRUMENT_NOTIFY:
		self->notify_port = (LV2_Atom_Sequence*)data;
		break;
	case INSTRUMENT_AUDIO_IN1:
		self->input_port1 = (float*)data;
		break;
	case INSTRUMENT_AUDIO_IN2:
		self->input_port2 = (float*)data;
		break;
	case INSTRUMENT_AUDIO_OUT1:
		self->output_port1 = (float*)data;
		break;
	case INSTRUMENT_AUDIO_OUT2:
		self->output_port2 = (float*)data;
		break;
	default:
		break;
	}
}

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            double                    rate,
            const char*               path,
            const LV2_Feature* const* features)
{
	// Allocate and initialise instance structure.
	Instrument* self = (Instrument*)malloc(sizeof(Instrument));
	if (!self) {
		return NULL;
	}
	memset(self, 0, sizeof(Instrument));

	lv2_log_trace(&self->logger, "Instrument coming up...\n");

	
	// Get host features
	for (int i = 0; features[i]; ++i) {
		if (!strcmp(features[i]->URI, LV2_URID__map)) {
			self->map = (LV2_URID_Map*)features[i]->data;
		} else if (!strcmp(features[i]->URI, LV2_WORKER__schedule)) {
			self->schedule = (LV2_Worker_Schedule*)features[i]->data;
		} else if (!strcmp(features[i]->URI, LV2_LOG__log)) {
			self->log = (LV2_Log_Log*)features[i]->data;
		}
	}
	if (!self->map) {
		lv2_log_error(&self->logger, "Missing feature urid:map\n");
		free(self);
		return 0;
	} else if (!self->schedule) {
		lv2_log_error(&self->logger, "Missing feature work:schedule\n");
		free(self);
		return 0;
	}

	// Map URIs and initialise forge/logger
	map_sampler_uris(self->map, &self->uris);
	lv2_atom_forge_init(&self->forge, self->map);
	lv2_log_logger_init(&self->logger, self->map, self->log);

	self->instrument = 0;

	return (LV2_Handle)self;
}

static void
cleanup(LV2_Handle instance)
{
	Instrument* self = (Instrument*)instance;
	
	if (self->instrument)
		free_instrument(self, self->instrument);
	free(self);
}

static void
run(LV2_Handle instance,
    uint32_t   sample_count)
{
	Instrument*     self        = (Instrument*)instance;
	InstrumentURIs* uris        = &self->uris;
#if 0
	sf_count_t   start_frame = 0;
	sf_count_t   pos         = 0;
	float*       output      = self->output_port;
#endif
	
	// Set up forge to write directly to notify output port.
	const uint32_t notify_capacity = self->notify_port->atom.size;
	lv2_atom_forge_set_buffer(&self->forge,
	                          (uint8_t*)self->notify_port,
	                          notify_capacity);

	// Start a sequence in the notify output port.
	lv2_atom_forge_sequence_head(&self->forge, &self->notify_frame, 0);

	// Read incoming events
	LV2_ATOM_SEQUENCE_FOREACH(self->control_port, ev) {
		self->frame_offset = ev->time.frames;
		if (ev->body.type == uris->midi_Event) {
#if 0
			const uint8_t* const msg = (const uint8_t*)(ev + 1);
			switch (lv2_midi_message_type(msg)) {
			case LV2_MIDI_MSG_NOTE_ON:
				start_frame = ev->time.frames;
				self->frame = 0;
				self->play  = true;
				break;
			default:
				break;
			}
#endif
		} else if (is_object_type(uris, ev->body.type)) {
			const LV2_Atom_Object* obj = (LV2_Atom_Object*)&ev->body;
			if (obj->body.otype == uris->patch_Set) {
				// Received a set message, send it to the worker.
				lv2_log_trace(&self->logger, "Queueing set message\n");
				self->schedule->schedule_work(self->schedule->handle,
				                              lv2_atom_total_size(&ev->body),
				                              &ev->body);
			} else {
				lv2_log_trace(&self->logger,
				              "Unknown object type %d\n", obj->body.otype);
			}
		} else {
			lv2_log_trace(&self->logger,
			              "Unknown event type %d\n", ev->body.type);
		}
	}
	
#if 0
	// Render the sample (possibly already in progress)
	if (self->play) {
		uint32_t       f  = self->frame;
		const uint32_t lf = self->sample->info.frames;

		for (pos = 0; pos < start_frame; ++pos) {
			output[pos] = 0;
		}

		for (; pos < sample_count && f < lf; ++pos, ++f) {
			output[pos] = self->sample->data[f];
		}

		self->frame = f;

		if (f == lf) {
			self->play = false;
		}
	}

	// Add zeros to end if sample not long enough (or not playing)
	for (; pos < sample_count; ++pos) {
		output[pos] = 0.0f;
	}
#endif
}

static LV2_State_Status
save(LV2_Handle                instance,
     LV2_State_Store_Function  store,
     LV2_State_Handle          handle,
     uint32_t                  flags,
     const LV2_Feature* const* features)
{
	lv2_log_trace(&((Instrument*)instance)->logger, "Saving instrument settings\n");

	Instrument* self = (Instrument*)instance;
	if (!self->instrument) {
		return LV2_STATE_SUCCESS;
	}

	LV2_State_Map_Path* map_path = NULL;
	for (int i = 0; features[i]; ++i) {
		if (!strcmp(features[i]->URI, LV2_STATE__mapPath)) {
			map_path = (LV2_State_Map_Path*)features[i]->data;
		}
	}

	char* apath = map_path->abstract_path(map_path->handle, self->instrument->path.c_str());

	store(handle,
	      self->uris.instrument,
	      apath,
	      self->instrument->path.length() + 1,
	      self->uris.atom_Path,
	      LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

	free(apath);

	return LV2_STATE_SUCCESS;
}

static LV2_State_Status
restore(LV2_Handle                  instance,
        LV2_State_Retrieve_Function retrieve,
        LV2_State_Handle            handle,
        uint32_t                    flags,
        const LV2_Feature* const*   features)
{
	lv2_log_trace(&((Instrument*)instance)->logger, "Restoring instrument settings\n");

	Instrument* self = (Instrument*)instance;

	size_t   size;
	uint32_t type;
	uint32_t valflags;

	const void* value = retrieve(
		handle,
		self->uris.instrument,
		&size, &type, &valflags);

	if (value) {
		const char* path = (const char*)value;
		lv2_log_trace(&self->logger, "Restoring file %s\n", path);
		free_instrument(self, self->instrument);
		self->instrument = load_instrument(self, path);
	}

	return LV2_STATE_SUCCESS;
}

static const void*
extension_data(const char* uri)
{
	static const LV2_State_Interface  state  = { save, restore };
	static const LV2_Worker_Interface worker = { work, work_response, NULL };
	if (!strcmp(uri, LV2_STATE__interface)) {
		return &state;
	} else if (!strcmp(uri, LV2_WORKER__interface)) {
		return &worker;
	}
	return NULL;
}

static const LV2_Descriptor descriptor = {
	INSTRUMENT_URI,
	instantiate,
	connect_port,
	NULL,  // activate,
	run,
	NULL,  // deactivate,
	cleanup,
	extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
	switch (index) {
	case 0:
		return &descriptor;
	default:
		return NULL;
	}
}
