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
#include <boost/iterator/iterator_concepts.hpp>
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

#include <execinfo.h>
#include <cxxabi.h>

const unsigned buffer_size = 1024;

static std::string
symbol_demangle (const std::string& l)
{
	int status;

	try {
		char* realname = abi::__cxa_demangle (l.c_str(), 0, 0, &status);
		std::string d (realname);
		free (realname);
	return d;
	} catch (std::exception) {

	}

	return l;
}
std::string demangle (std::string const & l)
{
	std::string::size_type const b = l.find_first_of ("(");

	if (b == std::string::npos) {
		return symbol_demangle (l);
	}

	std::string::size_type const p = l.find_last_of ("+");
	if (p == std::string::npos) {
	return symbol_demangle (l);
	}

	if ((p - b) <= 1) {
	return symbol_demangle (l);
	}

	std::string const fn = l.substr (b + 1, p - b - 1);

	return symbol_demangle (fn);
}

void stacktrace (std::ostream& out, int levels)
{
	void *array[200];
	size_t size;
	char **strings;
	size_t i;
		
	size = backtrace (array, 200);

	if (size) {
		strings = backtrace_symbols (array, size);
			
		if (strings) {

			for (i = 0; i < size && (levels == 0 || i < size_t(levels)); i++) {
				out << " " << demangle (strings[i]) << std::endl;
			}

			free (strings);
		}
	} else {
		out << "no stacktrace available!" << std::endl;
	}
}


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
	std::cout << "Building synth..." << std::endl;
	ladspam::synth_ptr the_synth(new ladspam::synth(sample_rate, control_period));
	
	for (int plugin_index = 0; plugin_index < synth_pb.plugins_size(); ++plugin_index)
	{
		ladspam_pb::Plugin plugin_pb = synth_pb.plugins(plugin_index);
		
		std::cout << "Adding plugin: " << the_synth->find_plugin_library(plugin_pb.label()) << " " << plugin_pb.label() << std::endl;
		
		the_synth->append_plugin
		(
			the_synth->find_plugin_library(plugin_pb.label()), 
			plugin_pb.label()
		);
		
		for (int value_index = 0; value_index < plugin_pb.values_size(); ++value_index)
		{
			ladspam_pb::Value value = plugin_pb.values(value_index);
			
			the_synth->set_port_value(plugin_index, value.port_index(), value.value());
		}
	}
	
	for (int connection_index = 0; connection_index < synth_pb.connections_size(); ++connection_index)
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
	float m_on_velocity;
	unsigned m_off_velocity;
	float m_note_frequency;
	unsigned m_start_frame;
	std::vector<ladspam::synth::buffer_ptr> m_port_buffers;
	std::vector<float *> m_port_buffers_raw;
	
	voice(unsigned control_period) :
		m_gate(0.0),
		m_note(0),
		m_on_velocity(0),
		m_off_velocity(0),
		m_note_frequency(0),
		m_start_frame(0)
	{
		{
			// Trigger
			ladspam::synth::buffer_ptr buffer(new std::vector<float>());
			buffer->resize(control_period);
			m_port_buffers.push_back(buffer);
			m_port_buffers_raw.push_back(&(*buffer.get())[0]);
		}

		{
			// Gate
			ladspam::synth::buffer_ptr buffer(new std::vector<float>());
			buffer->resize(control_period);
			m_port_buffers.push_back(buffer);
			m_port_buffers_raw.push_back(&(*buffer.get())[0]);
		}

		{
			// Velocity
			ladspam::synth::buffer_ptr buffer(new std::vector<float>());
			buffer->resize(control_period);
			m_port_buffers.push_back(buffer);
			m_port_buffers_raw.push_back(&(*buffer.get())[0]);
		}

		{
			// Frequency
			ladspam::synth::buffer_ptr buffer(new std::vector<float>());
			buffer->resize(control_period);
			m_port_buffers.push_back(buffer);
			m_port_buffers_raw.push_back(&(*buffer.get())[0]);
		}

		{
			ladspam::synth::buffer_ptr buffer(new std::vector<float>());
			buffer->resize(control_period);
			m_port_buffers.push_back(buffer);
			m_port_buffers_raw.push_back(&(*buffer.get())[0]);
		}
	}
};


struct MInstrument {
	ladspam::synth_ptr m_synth;
	std::vector<voice> m_voices;
	unsigned m_current_voice;
	std::vector<ladspam::synth::buffer_ptr> m_exposed_input_port_buffers;
	std::vector<ladspam::synth::buffer_ptr> m_exposed_output_port_buffers;
	std::string m_path;
	unsigned m_frame;
};


void expose_ports(MInstrument *instrument, ladspam_pb::Synth synth_pb, ladspam::synth_ptr the_synth)
{
	for (int port_index = 0; port_index < synth_pb.exposed_ports_size(); ++port_index)
	{
		ladspam_pb::Port port = synth_pb.exposed_ports(port_index);
		
		ladspamm::plugin_ptr the_plugin = the_synth->get_plugin(port.plugin_index())->the_plugin;
		
		if (the_plugin->port_is_input(port.port_index()))
		{
			ladspam::synth::buffer_ptr buffer(new std::vector<float>);
			
			buffer->resize(the_synth->buffer_size());
			
			instrument->m_exposed_input_port_buffers.push_back(buffer);
			
			the_synth->connect(port.plugin_index(), port.port_index(), buffer);
		}
		else
		{
			instrument->m_exposed_output_port_buffers.push_back(the_synth->get_buffer(port.plugin_index(), port.port_index()));
		}
	}
}


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
	std::vector<float*>      input_ports;
	std::vector<float*>      output_ports;

	// Forge frame for notify port (for writing worker replies)
	LV2_Atom_Forge_Frame notify_frame;

	// URIs
	InstrumentURIs uris;

	// Current position in run()
	uint32_t frame_offset;

	unsigned long frame;

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
	// stacktrace(std::cout, 15);

	std::cout << "Loading instrument " << path << std::endl;

	ladspam_pb::Instrument instrument_pb;

	try {
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
	} catch (...) {
		std::cout << "Failed to read instrument definition file" << std::endl;
		return 0;
	}

	MInstrument *instrument = 0;
	
	try {
		instrument  = new MInstrument;

		ladspam::synth_ptr synth = build_synth(instrument_pb.synth(), self->samplerate, buffer_size);
		std::cout << "Succeeded to load instrument" << std::endl;

		expose_ports(instrument, instrument_pb.synth(), synth);
		
		for (int voice_index = 0; voice_index < instrument_pb.number_of_voices(); ++voice_index)
		{
			instrument->m_voices.push_back(voice(buffer_size));
		}
		instrument->m_current_voice = 0;

		for (int connection_index = 0; connection_index < instrument_pb.connections_size(); ++connection_index)
		{
			ladspam_pb::Connection connection = instrument_pb.connections(connection_index);

			synth->connect
			(
				connection.sink_index(),
				connection.sink_port_index(),
				instrument->m_voices[connection.source_index()].m_port_buffers[connection.source_port_index()]
			);
		}
		
		instrument->m_synth = synth;
		instrument->m_path  = path;
		
		instrument->m_frame = 0;
		
		return instrument;
	} catch (std::exception &e) {
		std::cout << "Error loading instrument: " << e.what() << std::endl;
		delete instrument;
		return 0;
	}
}

static void
free_instrument(Instrument* self, MInstrument *instrument)
{
	if (instrument) {
		std::cout << "Freeing " << instrument->m_path << std::endl;
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
	std::cout << "Loading instrument - work" << std::endl;
	
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
	               self->instrument->m_path.c_str(),
	               self->instrument->m_path.length());

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
		self->input_ports[0] = (float*)data;
		break;
	case INSTRUMENT_AUDIO_IN2:
		self->input_ports[1] = (float*)data;
		break;
	case INSTRUMENT_AUDIO_OUT1:
		self->output_ports[0] = (float*)data;
		break;
	case INSTRUMENT_AUDIO_OUT2:
		self->output_ports[1] = (float*)data;
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
	self->samplerate = rate;
	self->input_ports.resize(2);
	self->output_ports.resize(2);

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

static void process(Instrument *instrument, unsigned nframes, unsigned offset)
{
	//std::cout << nframes << " " << offset << std::endl;
	
	unsigned number_of_chunks = nframes / buffer_size;
	unsigned remainder = nframes % buffer_size;
	
	unsigned number_of_input_ports = 
		std::min<unsigned>(2, instrument->instrument->m_exposed_input_port_buffers.size());
	
	unsigned number_of_output_ports = 
		std::min<unsigned>(2, instrument->instrument->m_exposed_output_port_buffers.size());
	
	for (unsigned chunk_index = 0; chunk_index < number_of_chunks; ++chunk_index)
	{
		for 
		(
			unsigned port_index = 0; 
			port_index < number_of_input_ports; 
			++port_index
		)
		{
			std::copy
			(
				instrument->input_ports[port_index] + offset + chunk_index * buffer_size, 
				instrument->input_ports[port_index] + offset + chunk_index * buffer_size + buffer_size, 
				instrument->instrument->m_exposed_input_port_buffers[port_index]->begin()
			);
		}
		
		instrument->instrument->m_synth->process(buffer_size);
		
		for 
		(
			unsigned port_index = 0; 
			port_index < number_of_output_ports; 
			++port_index
		)
		{
			std::copy
			(
				instrument->instrument->m_exposed_output_port_buffers[port_index]->begin(),
				instrument->instrument->m_exposed_output_port_buffers[port_index]->begin() + buffer_size,
				instrument->output_ports[port_index] + offset + chunk_index * buffer_size
			);
		}
	}
	
	for 
	(
		unsigned port_index = 0; 
		port_index < number_of_input_ports; 
		++port_index
	)
	{
		std::copy
		(
			instrument->input_ports[port_index] + offset + number_of_chunks * buffer_size, 
			instrument->input_ports[port_index] + offset + number_of_chunks * buffer_size + remainder, 
			instrument->instrument->m_exposed_input_port_buffers[port_index]->begin()
		);
	}
	
	instrument->instrument->m_synth->process(remainder);

	for 
	(
		unsigned port_index = 0; 
		port_index < number_of_output_ports; 
		++port_index
	)
	{
		std::copy
		(
			instrument->instrument->m_exposed_output_port_buffers[port_index]->begin(),
			instrument->instrument->m_exposed_output_port_buffers[port_index]->begin() + remainder,
			instrument->output_ports[port_index] + offset + number_of_chunks * buffer_size
		);
	}
}


float note_frequency(unsigned int note) 
{
	return (2.0 * 440.0 / 32.0) * pow(2, (((float)note - 9.0) / 12.0));
}

unsigned oldest_voice(MInstrument *instrument, unsigned frame)
{
	unsigned long minimum_age = frame - instrument->m_voices[0].m_start_frame;
	unsigned oldest_index = 0;
	
	for (unsigned voice_index = 1; voice_index < instrument->m_voices.size(); ++voice_index)
	{
		unsigned long age = frame - instrument->m_voices[voice_index].m_start_frame;
		if (age > minimum_age)
		{
			oldest_index = voice_index;
			minimum_age = age;
		}
	}
	
	return oldest_index;
}

int voice_playing_note(MInstrument *instrument, unsigned note)
{
	for (unsigned voice_index = 0; voice_index < instrument->m_voices.size(); ++voice_index)
	{
		if (instrument->m_voices[voice_index].m_note == note && instrument->m_voices[voice_index].m_gate > 0)
		{
			return voice_index;
		}
	}
	
	// UGLY
	return -1;
}

static void
run(LV2_Handle instance,
    uint32_t   sample_count)
{
	// std::cout << ".";
	
	Instrument*     self        = (Instrument*)instance;
	InstrumentURIs* uris        = &self->uris;
	unsigned offset             = 0;
	
	MInstrument *instrument     = self->instrument;
	
	// Set up forge to write directly to notify output port.
	const uint32_t notify_capacity = self->notify_port->atom.size;
	lv2_atom_forge_set_buffer(&self->forge,
	                          (uint8_t*)self->notify_port,
	                          notify_capacity);

	// Start a sequence in the notify output port.
	lv2_atom_forge_sequence_head(&self->forge, &self->notify_frame, 0);

	
	LV2_ATOM_SEQUENCE_FOREACH(self->control_port, ev) 
	{
		self->frame_offset = ev->time.frames;
		if (is_object_type(uris, ev->body.type)) 
		{
			const LV2_Atom_Object* obj = (LV2_Atom_Object*)&ev->body;
			
			if (obj->body.otype == uris->patch_Set) 
			{
				/* Received a set message, send it to the worker. */
				//print(self, self->uris.log_Trace, "Queueing set message\n");
				self->schedule->schedule_work
				(
					self->schedule->handle,
					lv2_atom_total_size(&ev->body),
					&ev->body
				);
			}
		}
	}

	int number_of_voices = instrument ? instrument->m_voices.size() : 0;

	if (instrument)
	{
		for 
		(
			int voice_index = 0; 
			voice_index < number_of_voices; 
			++voice_index
		)
		{
			std::vector<float *> &buffers = instrument->m_voices[voice_index].m_port_buffers_raw;
			
			float *trigger_buffer = buffers[0];
			std::fill
			(
				trigger_buffer,
				trigger_buffer + buffer_size,
				0.0f
			);
			
#if 0
			float *gate_buffer = buffers[1];
			std::fill
			(
				gate_buffer,
				gate_buffer + buffer_size,
				0.0f
			);

			float *velocity_buffer = buffers[2];
			std::fill
			(
				velocity_buffer,
				velocity_buffer + buffer_size,
				1.0f
			);

			float *freq_buffer = buffers[3];
			std::fill
			(
				freq_buffer,
				freq_buffer + buffer_size,
				4 * 440.0f
			);
#endif
		}
	}

	LV2_Atom_Event *ev = lv2_atom_sequence_begin(&(self->control_port)->body);

	
	unsigned chunk_index = 0;
	for (unsigned frame_index = 0; frame_index < sample_count; ++frame_index)
	{
		unsigned frame_in_chunk = frame_index % buffer_size;

		while(false == lv2_atom_sequence_is_end(&(self->control_port)->body, self->control_port->atom.size, ev) && ev->time.frames == frame_index)
		{
			std::cout << "ev" << std::endl;
			self->frame_offset = ev->time.frames;
			
			if (ev->body.type == uris->midi_Event) 
			{
				if (instrument)
				{
					const uint8_t* const msg = (const uint8_t*)(ev + 1);
					switch (lv2_midi_message_type(msg)) 
					{
						case LV2_MIDI_MSG_NOTE_ON:
						{
							const uint8_t *note = (const uint8_t*)(ev + 1) + 1;
							const uint8_t *velocity = (const uint8_t*)(ev + 1) + 2;
							
							std::cout << (int)*note << std::endl;
							unsigned the_voice = oldest_voice(instrument, ev->time.frames + instrument->m_frame);
							// std::cout << the_voice << std::endl;
							instrument->m_voices[the_voice].m_note = *note;
							instrument->m_voices[the_voice].m_on_velocity = *velocity / 128.0;
							instrument->m_voices[the_voice].m_note_frequency = note_frequency(*note);
							instrument->m_voices[the_voice].m_port_buffers_raw[0][frame_in_chunk] = 1;
							instrument->m_voices[the_voice].m_gate = 1;
							instrument->m_voices[the_voice].m_start_frame = instrument->m_frame + ev->time.frames;
							break;
						}
						case LV2_MIDI_MSG_NOTE_OFF:
						{
							const uint8_t *note = (const uint8_t*)(ev + 1) + 1;
							
							unsigned the_voice = voice_playing_note(instrument, *note);
							
							if (-1 != the_voice)
							{
								instrument->m_voices[the_voice].m_gate = 0;
							}
#if 0
							unsigned the_voice = oldest_voice(instrument, ev->time.frames + instrument->m_frame);
							std::cout << the_voice << std::endl;
							instrument->m_voices[the_voice].m_port_buffers_raw[0][frame_in_chunk] = 1;
							instrument->m_voices[the_voice].m_gate = 1;
							instrument->m_voices[the_voice].m_start_frame = instrument->m_frame + ev->time.frames;
							break;
#endif
						}
						default:
							break;
					}
				}
			} 
			ev = lv2_atom_sequence_next(ev);
		}
		
		for 
		(
			int voice_index = 0; 
			voice_index < number_of_voices; 
			++voice_index
		)
		{
			instrument->m_voices[voice_index].m_port_buffers_raw[1][frame_in_chunk] = instrument->m_voices[voice_index].m_gate;
			
			instrument->m_voices[voice_index].m_port_buffers_raw[2][frame_in_chunk] = instrument->m_voices[voice_index].m_on_velocity;

			instrument->m_voices[voice_index].m_port_buffers_raw[3][frame_in_chunk] = instrument->m_voices[voice_index].m_note_frequency;
		}
		
		if (0 == (frame_index + 1) % buffer_size || sample_count == frame_index + 1)
		{
			if (instrument)
			{
				process(self, frame_index % buffer_size + 1, chunk_index * buffer_size);
			}
			++chunk_index;
		}
	}
	
	if (instrument)
	{
		instrument->m_frame += sample_count;
	}
}

static LV2_State_Status
save(LV2_Handle                instance,
     LV2_State_Store_Function  store,
     LV2_State_Handle          handle,
     uint32_t                  flags,
     const LV2_Feature* const* features)
{
	std::cout << "Saving instrument settings..." << std::endl;

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

	char* apath = map_path->abstract_path(map_path->handle, self->instrument->m_path.c_str());

	store(handle,
	      self->uris.instrument,
	      apath,
	      self->instrument->m_path.length() + 1,
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
	std::cout << "Restoring instrument settings..." << std::endl;
	
	// stacktrace(std::cout, 15);

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
		std::cout << "Restoring file " <<  path << std::endl;
		if (self->instrument)
		{
			free_instrument(self, self->instrument);
		}
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
