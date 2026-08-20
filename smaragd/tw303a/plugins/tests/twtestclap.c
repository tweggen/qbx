/* twtestclap — an in-repo CLAP plugin used as a BUILD-TIME TEST FIXTURE.
 *
 * proposal 08 M1, extended by proposal 37 P2. Without this, the only way to
 * exercise the real CLAP load path (LoadLibrary/dlopen -> clap_entry -> factory
 * -> activate -> process -> params -> events -> state) is to install a
 * third-party plugin on the machine running the tests, which no CI and no fresh
 * checkout can be asked to do. This module is built as a MODULE library named
 * twtestclap.clap and its absolute path is handed to plugins_test as a compile
 * definition.
 *
 * It is written in C on purpose: a C DLL has no libstdc++/libgcc import, so it
 * loads regardless of where the test binary runs from or what is on PATH.
 *
 * FIVE entry points:
 *
 *   tw.test.clap.gain         2 in / 2 out effect     (M1)
 *   tw.test.clap.stereoskew   2 in / 2 out effect     (M3)
 *   tw.test.clap.sine         0 in / stereo + aux out INSTRUMENT   (37 P2)
 *   tw.test.clap.arp          note in / note out      (37 P2)
 *   tw.test.clap.gui          gain + a WINDOWLESS clap.gui          (33 M6)
 *
 * --- tw.test.clap.gain -------------------------------------------------------
 *
 *   id 0  "Gain"              0 .. 4, default 1      out = in * gain
 *   id 1  "Report Block Size" 0 .. 1, default 0, stepped
 *                             when >= 0.5, every output sample is set to
 *                             (float)frames_count instead of the gain result.
 *                             That is how the host-side chunking test observes
 *                             the block size the plugin actually saw.
 *   id 2  "Clip Threshold"    0 .. 4, default 0 (off)
 *                             when > 0, the gained sample is HARD CLIPPED to
 *                             +/- threshold. Order-sensitive by construction:
 *                             gain-then-clip and clip-then-gain give different
 *                             audio, which is what proposal 37 P3a's fader-move
 *                             ORDER case discriminates with.
 *
 *   NOTE ON THE ID. The brief for P2 says "param id 1 clipThreshold". Id 1 was
 *   already taken by "Report Block Size" (M1), which the host-chunking gate
 *   reads, so the clipper is id 2. Renumbering the existing one would have moved
 *   a live gate's parameter out from under it for no benefit. Recorded in
 *   plan/STATE.md so P3a's case quotes the right id.
 *
 * Parameter values are applied AT THEIR EVENT TIME, not at the top of the block
 * (proposal 37 AC2): the plugin renders the block in segments split at each
 * parameter event. With no mid-block event that is one segment and exactly the
 * arithmetic M1 had, so the effect goldens do not move.
 *
 * clap.state stores the two M1 parameters as little-endian doubles, and appends
 * the clip threshold ONLY when it is non-zero. That keeps a default instance's
 * blob byte-identical to the M1 one — plugin_slot_roundtrip.qxa asserts the
 * exact base64 of a saved chunk — while still persisting the clipper when a
 * project uses it. The loader accepts either length.
 *
 * Deliberate strictness: if process() is ever handed more frames than the host
 * promised at activate(), the plugin returns CLAP_PROCESS_ERROR instead of
 * overrunning. A host-side chunking regression therefore fails loudly.
 *
 * --- tw.test.clap.stereoskew (M3) --------------------------------------------
 *
 * Same params, same ports, but a DEFAULT behaviour that is deliberately both
 * channel-asymmetric AND cross-channel:
 *
 *   out[0] = in[0] * 0.5 * gain  +  in[1] * 1.0 * gain
 *   out[c] = in[c] * 0.5 * gain                          (c >= 1)
 *
 * Two properties are being bought here, and both matter because a qxa action
 * script cannot set a parameter or restore a state chunk before M5 — only
 * DEFAULT behaviour is reachable from a headless render:
 *
 *  - out[0] depends on in[1]. Feed it two identical mono buses (which is what a
 *    track does) and channel 0 comes out at 1.5x. If input 1 were SILENT — the
 *    pre-M3 bug, where a chain built with nBusses == 1 wired only port 0 — it
 *    would come out at 0.5x, and with no plugin at all at 1.0x. Three bands far
 *    enough apart for an RMS assertion to tell them apart.
 *  - out[1] != out[0], so once the sink really is multi-channel the same
 *    fixture proves the per-bus taps carry distinct audio.
 *
 * --- tw.test.clap.sine (proposal 37 P2, design §5.3) -------------------------
 *
 * The reference INSTRUMENT. Features INSTRUMENT|SYNTHESIZER; 0 audio in; a
 * stereo MAIN out plus a mono AUX out (so the descriptor's nOutBuses / aux
 * discovery has something real to report). One note input port declaring
 * CLAP|MIDI with CLAP preferred, so the host's dialect negotiation is exercised.
 *
 * 16 sine voices, oldest-steals, amp = velocity, NO envelope and instant on/off
 * — which is exactly what makes it gateable: the RMS of a held note is
 * velocity/sqrt(2) * gain in closed form, the fundamental is exactly the key's
 * frequency, and silence before the note-on and after the note-off is EXACT
 * (peak < 1e-6), not "small". A voice's phase starts at 0 on note-on, so two
 * runs of "reset, note-on, render N" are byte-identical (AC5).
 *
 * It pushes CLAP_EVENT_NOTE_END on every note-off, so the host's event-OUT path
 * has a producer that is not the arpeggiator.
 *
 * PARAM ID 3, "Stereo Skew" (stepped, default 0 = OFF, added 2026-08-17 for the
 * proposal 37 stereo gates). Off, every main channel carries the same sample -
 * which is what every assertion written before this date measured, and is why
 * the default may never move. On, channels 1.. of the MAIN out are at HALF
 * amplitude, so the rendered file's channel relation is a closed form:
 *
 *     rms(ch0) = velocity/sqrt(2) * gain      rms(ch1) = rms(ch0) / 2
 *
 * An instrument whose outputs are identical cannot distinguish a wide sink from
 * one that duplicates channel 0, which is exactly what qxa.instrument_stereo_
 * render needs to tell apart; the aux out is untouched (still main * 0.5).
 *
 * CLAP_PROCESS_ERROR on an over-size block (as the gain fixture) and on a
 * WILDCARD note-on (key < 0): a host that forwards a wildcard to an instrument
 * has lost the note's identity, and failing loudly beats playing a wrong note.
 *
 * --- tw.test.clap.arp (proposal 37 P2, AC4) ----------------------------------
 *
 * A note-in / note-out plugin with NO audio ports. It holds the keys that are
 * down and emits a NoteOn on a fixed 4096-frame grid with a NoteOff 2048 frames
 * later, through out_events->try_push — the host's twEventOut plumbing under a
 * producer whose output count has a closed form:
 *
 *     notes-on over N frames from a key held at frame 0 = ceil(N / 4096)
 *     each is paired with exactly one note-off (2048 < 4096, so no overlap)
 *
 * The grid counts from reset(), in ABSOLUTE frames, so it is independent of how
 * the host chunks the block.
 */

#include <clap/clap.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* M_PI is not in ISO C and MinGW hides it under _USE_MATH_DEFINES; spelling it
 * out keeps the fixture free of feature-test macros. */
#define TW_PI 3.14159265358979323846

#define TW_TESTCLAP_ID      "tw.test.clap.gain"
#define TW_TESTCLAP_SKEW_ID "tw.test.clap.stereoskew"
#define TW_TESTCLAP_SINE_ID "tw.test.clap.sine"
#define TW_TESTCLAP_ARP_ID  "tw.test.clap.arp"
#define TW_TESTCLAP_GUI_ID  "tw.test.clap.gui"

/* Which behaviour this instance has. tw.test.clap.gui shares GAIN's, and is
 * told apart by hasGui rather than by a kind of its own. */
enum tw_kind { TW_KIND_GAIN = 0, TW_KIND_SKEW = 1, TW_KIND_SINE = 2, TW_KIND_ARP = 3 };

#define TW_SINE_VOICES 16
#define TW_ARP_GRID    4096u
#define TW_ARP_GATE    2048u

static const char *const s_features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
                                          CLAP_PLUGIN_FEATURE_UTILITY,
                                          CLAP_PLUGIN_FEATURE_STEREO,
                                          NULL };

static const char *const s_features_inst[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                               CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                               CLAP_PLUGIN_FEATURE_STEREO,
                                               NULL };

static const char *const s_features_note[] = { CLAP_PLUGIN_FEATURE_NOTE_EFFECT,
                                               NULL };

static const clap_plugin_descriptor_t s_desc_skew = {
   .clap_version = CLAP_VERSION_INIT,
   .id           = TW_TESTCLAP_SKEW_ID,
   .name         = "Smaragd Test Stereo Skew",
   .vendor       = "Smaragd",
   .url          = "https://github.com/tweggen/qbx",
   .manual_url   = "",
   .support_url  = "",
   .version      = "1.0.0",
   .description  = "Test fixture: gain with every channel above the first at -6 dB",
   .features     = s_features,
};

static const clap_plugin_descriptor_t s_desc = {
   .clap_version = CLAP_VERSION_INIT,
   .id           = TW_TESTCLAP_ID,
   .name         = "Smaragd Test Gain",
   .vendor       = "Smaragd",
   .url          = "https://github.com/tweggen/qbx",
   .manual_url   = "",
   .support_url  = "",
   .version      = "1.0.0",
   .description  = "Test fixture: stereo gain with a block-size reporter and a clipper",
   .features     = s_features,
};

static const clap_plugin_descriptor_t s_desc_sine = {
   .clap_version = CLAP_VERSION_INIT,
   .id           = TW_TESTCLAP_SINE_ID,
   .name         = "Smaragd Test Sine",
   .vendor       = "Smaragd",
   .url          = "https://github.com/tweggen/qbx",
   .manual_url   = "",
   .support_url  = "",
   .version      = "1.0.0",
   .description  = "Test fixture: 16-voice envelope-less sine instrument",
   .features     = s_features_inst,
};

/* tw.test.clap.gui (proposal 33 M6) - the gain fixture with a clap.gui
 * extension bolted on. It is a SEPARATE ENTRY POINT rather than a flag on
 * tw.test.clap.gain precisely because supportsNativeEditor() is observable:
 * giving gain a GUI would flip qxa.plugin_ui_strip_and_editor's fallback
 * assertion from "generic" to "native" and make a headless run try to open a
 * window.
 *
 * IT CREATES NO WINDOW. create() allocates nothing and set_parent() accepts
 * any handle, which is what lets the whole parameter path be gated with no
 * window system at all - the substance of M6 is where a GUI edit GOES, not
 * where it is drawn. Two triggers stand in for a user turning a knob:
 *
 *   show()          -> queues Begin/2.5/End on param 0 and asks the host to
 *                      flush. This is the route a real plugin uses while
 *                      nothing is rendering, and the one that does not work
 *                      unless the host services request_flush.
 *   set_size(w,h)   -> queues Begin/0.75/End on param 0 WITHOUT asking for a
 *                      flush, so it can only come out through process().
 *
 * show() also calls host_gui->request_resize(300, 200), so the resize half of
 * twEditorFeedback has a closed form too.
 */
static const clap_plugin_descriptor_t s_desc_gui = {
   .clap_version = CLAP_VERSION_INIT,
   .id           = TW_TESTCLAP_GUI_ID,
   .name         = "Smaragd Test GUI",
   .vendor       = "Smaragd",
   .url          = "https://github.com/tweggen/qbx",
   .manual_url   = "",
   .support_url  = "",
   .version      = "1.0.0",
   .description  = "Test fixture: gain with a windowless clap.gui extension",
   .features     = s_features,
};

static const clap_plugin_descriptor_t s_desc_arp = {
   .clap_version = CLAP_VERSION_INIT,
   .id           = TW_TESTCLAP_ARP_ID,
   .name         = "Smaragd Test Arp",
   .vendor       = "Smaragd",
   .url          = "https://github.com/tweggen/qbx",
   .manual_url   = "",
   .support_url  = "",
   .version      = "1.0.0",
   .description  = "Test fixture: note in / note out on a fixed frame grid",
   .features     = s_features_note,
};

typedef struct {
   int      active;    /* 1 while sounding */
   int      key;
   int32_t  noteId;
   int16_t  channel;
   int16_t  port;
   double   velocity;
   double   phase;     /* 0 .. 1 */
   double   phaseInc;
   uint64_t age;       /* for oldest-steals */
} tw_voice_t;

typedef struct {
   clap_plugin_t      base;
   const clap_host_t *host;

   int      kind;
   double   gain;
   double   report;
   double   clip;      /* 0 = off; > 0 = hard clip at +/- clip AFTER the gain */
   double   skew;      /* SINE only: 0 = off; >= 0.5 = right channels at 0.5x */
   uint32_t maxFrames;
   double   sampleRate;
   int      active;

   /* sine */
   tw_voice_t voices[TW_SINE_VOICES];
   uint64_t   voiceAge;

   /* arp */
   int      heldKeys[TW_SINE_VOICES];
   int16_t  heldChans[TW_SINE_VOICES];
   int      nHeld;
   uint64_t arpFrame;      /* absolute frames since reset */
   int      arpOffPending; /* 1 when an off is scheduled */
   uint64_t arpOffAt;      /* absolute frame of the pending off */
   int      arpOffKey;
   int16_t  arpOffChan;
   int32_t  arpOffId;
   int32_t  arpNextId;

   /* gui (proposal 33 M6) */
   int      hasGui;
   int      guiCreated;
   int      guiFloating;
   int      guiShown;
   int      guiParented;
   double   guiScale;
   uint32_t guiW, guiH;
   /* A queued "the user turned a knob" gesture: emitted, in full, by the next
    * flush() or process(). 0 = nothing queued. */
   int      guiPendingEdit;
   double   guiPendingValue;
} tw_testclap_t;

/* ---------------------------------------------------------------- params */

static uint32_t tc_params_count( const clap_plugin_t *p )
{
   const tw_testclap_t *self = (const tw_testclap_t *)p->plugin_data;
   if( self->kind == TW_KIND_ARP )
      return 0;
   if( self->kind == TW_KIND_SINE )
      return 2;   /* Gain, Stereo Skew */
   return 3;      /* Gain, Report Block Size, Clip Threshold */
}

static bool tc_params_get_info( const clap_plugin_t *p, uint32_t index,
                                clap_param_info_t *info )
{
   const tw_testclap_t *self = (const tw_testclap_t *)p->plugin_data;
   memset( info, 0, sizeof( *info ) );
   if( index == 0 ) {
      info->id            = 0;
      info->flags         = CLAP_PARAM_IS_AUTOMATABLE;
      info->min_value     = 0.0;
      info->max_value     = 4.0;
      info->default_value = 1.0;
      strncpy( info->name, "Gain", CLAP_NAME_SIZE - 1 );
      return true;
   }
   if( self->kind == TW_KIND_SINE && index == 1 ) {
      /* STEREO SKEW (proposal 37 stereo gates, 2026-08-17). OFF by default, so
       * every render made before this parameter existed is byte-identical: the
       * two main channels stay the same sample, which is what instrument_sine_
       * render.qxa's channel-0 closed form was written against. Switched ON the
       * right channels drop to HALF amplitude, which is a closed form too
       * (rms ch1 = rms ch0 / 2) and is what makes a channel claim assertable
       * from a FILE at all - an instrument whose two outputs are identical
       * cannot tell a wide sink from a duplicating one. */
      info->id            = 3;
      info->flags         = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
      info->min_value     = 0.0;
      info->max_value     = 1.0;
      info->default_value = 0.0;   /* off */
      strncpy( info->name, "Stereo Skew", CLAP_NAME_SIZE - 1 );
      return true;
   }
   if( self->kind == TW_KIND_SINE || self->kind == TW_KIND_ARP )
      return false;
   if( index == 1 ) {
      info->id            = 1;
      info->flags         = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
      info->min_value     = 0.0;
      info->max_value     = 1.0;
      info->default_value = 0.0;
      strncpy( info->name, "Report Block Size", CLAP_NAME_SIZE - 1 );
      return true;
   }
   if( index == 2 ) {
      info->id            = 2;
      info->flags         = CLAP_PARAM_IS_AUTOMATABLE;
      info->min_value     = 0.0;
      info->max_value     = 4.0;
      info->default_value = 0.0;   /* off */
      strncpy( info->name, "Clip Threshold", CLAP_NAME_SIZE - 1 );
      return true;
   }
   return false;
}

static bool tc_params_get_value( const clap_plugin_t *p, clap_id id, double *out )
{
   const tw_testclap_t *self = (const tw_testclap_t *)p->plugin_data;
   if( id == 0 ) { *out = self->gain;   return true; }
   if( self->kind == TW_KIND_SINE && id == 3 ) { *out = self->skew; return true; }
   if( self->kind == TW_KIND_SINE || self->kind == TW_KIND_ARP )
      return false;
   if( id == 1 ) { *out = self->report; return true; }
   if( id == 2 ) { *out = self->clip;   return true; }
   return false;
}

static bool tc_params_value_to_text( const clap_plugin_t *p, clap_id id, double value,
                                     char *out, uint32_t cap )
{
   (void)p;
   (void)id;
   if( cap == 0 )
      return false;
   /* No float formatting: keep the DLL free of stdio. */
   long whole = (long)( value * 100.0 );
   char tmp[32];
   int  n = 0;
   int  neg = whole < 0;
   if( neg ) whole = -whole;
   do { tmp[n++] = (char)( '0' + (int)( whole % 10 ) ); whole /= 10; } while( whole && n < 30 );
   if( neg && n < 31 ) tmp[n++] = '-';
   uint32_t k = 0;
   while( n > 0 && k + 1 < cap ) out[k++] = tmp[--n];
   out[k] = '\0';
   return true;
}

static bool tc_params_text_to_value( const clap_plugin_t *p, clap_id id,
                                     const char *text, double *out )
{
   (void)p; (void)id; (void)text; (void)out;
   return false;
}

static void tc_apply_param( tw_testclap_t *self, clap_id id, double value )
{
   if( id == 0 ) self->gain   = value;
   if( id == 1 ) self->report = value;
   if( id == 2 ) self->clip   = value;
   if( id == 3 ) self->skew   = value;
}

static void tc_apply_events( tw_testclap_t *self, const clap_input_events_t *in )
{
   if( !in || !in->size || !in->get )
      return;
   const uint32_t n = in->size( in );
   for( uint32_t i = 0; i < n; ++i ) {
      const clap_event_header_t *h = in->get( in, i );
      if( !h || h->space_id != CLAP_CORE_EVENT_SPACE_ID )
         continue;
      if( h->type != CLAP_EVENT_PARAM_VALUE )
         continue;
      const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
      tc_apply_param( self, e->param_id, e->value );
   }
}

/* Emit the queued GUI gesture into out_events, bracketed the way a real plugin
 * brackets a knob drag. Called from BOTH routes a plugin has - flush() and
 * process() - because that is the whole point of the fixture. */
static void tc_gui_emit_pending( tw_testclap_t *self, const clap_output_events_t *out )
{
   if( !self->guiPendingEdit || !out || !out->try_push )
      return;
   self->guiPendingEdit = 0;

   clap_event_param_gesture_t g;
   memset( &g, 0, sizeof( g ) );
   g.header.size     = sizeof( g );
   g.header.time     = 0;
   g.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
   g.header.type     = CLAP_EVENT_PARAM_GESTURE_BEGIN;
   g.param_id        = 0;
   out->try_push( out, &g.header );

   clap_event_param_value_t v;
   memset( &v, 0, sizeof( v ) );
   v.header.size     = sizeof( v );
   v.header.time     = 0;
   v.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
   v.header.type     = CLAP_EVENT_PARAM_VALUE;
   v.param_id        = 0;
   v.note_id         = -1;
   v.port_index      = -1;
   v.channel         = -1;
   v.key             = -1;
   v.value           = self->guiPendingValue;
   out->try_push( out, &v.header );

   g.header.type = CLAP_EVENT_PARAM_GESTURE_END;
   out->try_push( out, &g.header );
}

static void tc_params_flush( const clap_plugin_t *p, const clap_input_events_t *in,
                             const clap_output_events_t *out )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;
   tc_apply_events( self, in );
   tc_gui_emit_pending( self, out );
}

static const clap_plugin_params_t s_params = {
   .count         = tc_params_count,
   .get_info      = tc_params_get_info,
   .get_value     = tc_params_get_value,
   .value_to_text = tc_params_value_to_text,
   .text_to_value = tc_params_text_to_value,
   .flush         = tc_params_flush,
};

/* ----------------------------------------------------------- audio ports */

static uint32_t tc_ports_count( const clap_plugin_t *p, bool is_input )
{
   const tw_testclap_t *self = (const tw_testclap_t *)p->plugin_data;
   if( self->kind == TW_KIND_ARP )
      return 0;                       /* a note effect makes no sound */
   if( self->kind == TW_KIND_SINE )
      return is_input ? 0u : 2u;      /* an instrument: main stereo + aux mono */
   return 1;
}

static bool tc_ports_get( const clap_plugin_t *p, uint32_t index, bool is_input,
                          clap_audio_port_info_t *info )
{
   const tw_testclap_t *self = (const tw_testclap_t *)p->plugin_data;
   memset( info, 0, sizeof( *info ) );
   info->in_place_pair = CLAP_INVALID_ID;

   if( self->kind == TW_KIND_ARP )
      return false;

   if( self->kind == TW_KIND_SINE ) {
      if( is_input )
         return false;
      if( index == 0 ) {
         info->id            = 1;
         info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
         info->channel_count = 2;
         info->port_type     = CLAP_PORT_STEREO;
         strncpy( info->name, "Main Out", CLAP_NAME_SIZE - 1 );
         return true;
      }
      if( index == 1 ) {
         /* The AUX out. Nothing consumes it yet (proposal 37 §5.4 routes aux
          * outs to return tracks in P9) — it exists so the descriptor's
          * nOutBuses / outBusChannels have something other than 1 to report,
          * and so a host that mis-sizes its per-port scratch is caught. */
         info->id            = 2;
         info->flags         = 0;
         info->channel_count = 1;
         info->port_type     = CLAP_PORT_MONO;
         strncpy( info->name, "Aux Out", CLAP_NAME_SIZE - 1 );
         return true;
      }
      return false;
   }

   if( index != 0 )
      return false;
   info->id            = is_input ? 0 : 1;
   info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
   info->channel_count = 2;
   info->port_type     = CLAP_PORT_STEREO;
   strncpy( info->name, is_input ? "Main In" : "Main Out", CLAP_NAME_SIZE - 1 );
   return true;
}

static const clap_plugin_audio_ports_t s_ports = {
   .count = tc_ports_count,
   .get   = tc_ports_get,
};

/* ------------------------------------------------------------ note ports */

static uint32_t tc_note_ports_count( const clap_plugin_t *p, bool is_input )
{
   const tw_testclap_t *self = (const tw_testclap_t *)p->plugin_data;
   if( self->kind == TW_KIND_SINE )
      return is_input ? 1u : 0u;
   if( self->kind == TW_KIND_ARP )
      return 1u;                      /* one in AND one out */
   return 0u;
}

static bool tc_note_ports_get( const clap_plugin_t *p, uint32_t index, bool is_input,
                               clap_note_port_info_t *info )
{
   const tw_testclap_t *self = (const tw_testclap_t *)p->plugin_data;
   if( index != 0 )
      return false;
   if( self->kind != TW_KIND_SINE && self->kind != TW_KIND_ARP )
      return false;
   if( self->kind == TW_KIND_SINE && !is_input )
      return false;

   memset( info, 0, sizeof( *info ) );
   info->id = is_input ? 0 : 1;
   /* Both dialects offered, CLAP preferred — so the host's negotiation has a
    * real choice to make, and a host that picks MIDI when CLAP is preferred is
    * visible (the fixture then never sees a note_id). */
   info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
   info->preferred_dialect  = CLAP_NOTE_DIALECT_CLAP;
   strncpy( info->name, is_input ? "Note In" : "Note Out", CLAP_NAME_SIZE - 1 );
   return true;
}

static const clap_plugin_note_ports_t s_note_ports = {
   .count = tc_note_ports_count,
   .get   = tc_note_ports_get,
};

/* ----------------------------------------------------------------- tail */

static uint32_t tc_tail_get( const clap_plugin_t *p )
{
   const tw_testclap_t *self = (const tw_testclap_t *)p->plugin_data;
   /* The sine has no envelope, so it stops the instant its last note ends —
    * a zero tail, honestly reported. The effects have none either. */
   (void)self;
   return 0;
}

static const clap_plugin_tail_t s_tail = { .get = tc_tail_get };

/* ----------------------------------------------------------------- state */

static void tc_put_double( uint8_t *p, double v )
{
   /* All targets we build for are little-endian; memcpy keeps it aligned-safe. */
   memcpy( p, &v, sizeof( double ) );
}

static bool tc_write_all( const clap_ostream_t *os, const uint8_t *buf, size_t n )
{
   uint64_t written = 0;
   while( written < n ) {
      int64_t k = os->write( os, buf + written, n - written );
      if( k <= 0 )
         return false;
      written += (uint64_t)k;
   }
   return true;
}

static bool tc_state_save( const clap_plugin_t *p, const clap_ostream_t *os )
{
   const tw_testclap_t *self = (const tw_testclap_t *)p->plugin_data;

   if( self->kind == TW_KIND_SINE ) {
      /* The skew is appended ONLY when it is on, exactly as the gain fixture's
       * clip threshold is: a default instrument's blob stays the 8 bytes every
       * project written before 2026-08-17 carries. */
      uint8_t one[16];
      tc_put_double( one, self->gain );
      if( self->skew == 0.0 )
         return tc_write_all( os, one, 8 );
      tc_put_double( one + 8, self->skew );
      return tc_write_all( os, one, sizeof( one ) );
   }

   uint8_t buf[24];
   tc_put_double( buf,     self->gain );
   tc_put_double( buf + 8, self->report );

   /* The clip threshold is appended ONLY when it is set. A default instance's
    * blob is therefore byte-identical to the pre-36 one, which is what keeps
    * plugin_slot_roundtrip.qxa's exact-base64 assertion true. */
   size_t n = 16;
   if( self->clip != 0.0 ) {
      tc_put_double( buf + 16, self->clip );
      n = 24;
   }
   return tc_write_all( os, buf, n );
}

static bool tc_state_load( const clap_plugin_t *p, const clap_istream_t *is )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;

   if( self->kind == TW_KIND_SINE ) {
      uint8_t one[8];
      uint64_t got = 0;
      while( got < sizeof( one ) ) {
         int64_t k = is->read( is, one + got, sizeof( one ) - got );
         if( k <= 0 ) return false;
         got += (uint64_t)k;
      }
      memcpy( &self->gain, one, sizeof( double ) );

      /* The optional second value. A short blob simply leaves the skew off. */
      uint8_t  more[8];
      uint64_t extra = 0;
      while( extra < sizeof( more ) ) {
         int64_t k = is->read( is, more + extra, sizeof( more ) - extra );
         if( k <= 0 ) break;
         extra += (uint64_t)k;
      }
      self->skew = 0.0;
      if( extra == sizeof( more ) )
         memcpy( &self->skew, more, sizeof( double ) );
      return true;
   }

   uint8_t  buf[16];
   uint64_t read = 0;
   while( read < sizeof( buf ) ) {
      int64_t n = is->read( is, buf + read, sizeof( buf ) - read );
      if( n <= 0 )
         return false;
      read += (uint64_t)n;
   }
   memcpy( &self->gain,   buf,     sizeof( double ) );
   memcpy( &self->report, buf + 8, sizeof( double ) );

   /* The optional third value. A short blob (every project written before
    * proposal 37) simply leaves the clipper off. */
   uint8_t  extra[8];
   uint64_t got = 0;
   while( got < sizeof( extra ) ) {
      int64_t n = is->read( is, extra + got, sizeof( extra ) - got );
      if( n <= 0 )
         break;
      got += (uint64_t)n;
   }
   self->clip = 0.0;
   if( got == sizeof( extra ) )
      memcpy( &self->clip, extra, sizeof( double ) );
   return true;
}

static const clap_plugin_state_t s_state = {
   .save = tc_state_save,
   .load = tc_state_load,
};

/* --------------------------------------------------------------- latency */

static uint32_t tc_latency_get( const clap_plugin_t *p )
{
   (void)p;
   return 0;
}

static const clap_plugin_latency_t s_latency = { .get = tc_latency_get };

/* ------------------------------------------------------------ sine voices */

static double tc_key_to_hz( int key )
{
   return 440.0 * pow( 2.0, ( (double)key - 69.0 ) / 12.0 );
}

static void tc_voices_reset( tw_testclap_t *self )
{
   memset( self->voices, 0, sizeof( self->voices ) );
   self->voiceAge = 0;
}

/* Oldest-steals. Returns the slot a new voice should use. */
static tw_voice_t *tc_voice_alloc( tw_testclap_t *self )
{
   tw_voice_t *oldest = &self->voices[0];
   for( int i = 0; i < TW_SINE_VOICES; ++i ) {
      if( !self->voices[i].active )
         return &self->voices[i];
      if( self->voices[i].age < oldest->age )
         oldest = &self->voices[i];
   }
   return oldest;
}

static void tc_note_on( tw_testclap_t *self, const clap_event_note_t *n )
{
   tw_voice_t *v = tc_voice_alloc( self );
   v->active   = 1;
   v->key      = n->key;
   v->noteId   = n->note_id;
   v->channel  = n->channel;
   v->port     = n->port_index;
   v->velocity = n->velocity;
   v->phase    = 0.0;   /* deterministic: AC5 compares two runs byte for byte */
   v->phaseInc = tc_key_to_hz( n->key ) / self->sampleRate;
   v->age      = ++self->voiceAge;
}

/* Ends every voice matching the event's (note_id, port, channel, key) with the
 * CLAP wildcard rule (-1 matches anything), and reports each one back to the
 * host as CLAP_EVENT_NOTE_END. */
static void tc_note_off( tw_testclap_t *self, const clap_event_note_t *n,
                         const clap_output_events_t *out, uint32_t time )
{
   for( int i = 0; i < TW_SINE_VOICES; ++i ) {
      tw_voice_t *v = &self->voices[i];
      if( !v->active )
         continue;
      if( n->note_id >= 0 && v->noteId != n->note_id ) continue;
      if( n->note_id < 0 ) {
         if( n->key >= 0 && v->key != n->key )             continue;
         if( n->channel >= 0 && v->channel != n->channel ) continue;
         if( n->port_index >= 0 && v->port != n->port_index ) continue;
      }
      v->active = 0;

      if( out && out->try_push ) {
         clap_event_note_t e;
         memset( &e, 0, sizeof( e ) );
         e.header.size     = sizeof( e );
         e.header.time     = time;
         e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
         e.header.type     = CLAP_EVENT_NOTE_END;
         e.note_id         = v->noteId;
         e.port_index      = v->port;
         e.channel         = v->channel;
         e.key             = v->key;
         e.velocity        = 0.0;
         out->try_push( out, &e.header );
      }
   }
}

/* ---------------------------------------------------------------- plugin */

static bool tc_init( const clap_plugin_t *p )
{
   (void)p;
   return true;
}

static void tc_destroy( const clap_plugin_t *p )
{
   free( p->plugin_data );
}

static bool tc_activate( const clap_plugin_t *p, double sample_rate,
                         uint32_t min_frames, uint32_t max_frames )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;
   (void)min_frames;
   if( max_frames == 0 )
      return false;
   self->maxFrames  = max_frames;
   self->sampleRate = sample_rate > 0.0 ? sample_rate : 48000.0;
   self->active     = 1;
   return true;
}

static void tc_deactivate( const clap_plugin_t *p )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;
   self->active = 0;
}

static bool tc_start_processing( const clap_plugin_t *p )
{
   (void)p;
   return true;
}

static void tc_stop_processing( const clap_plugin_t *p )
{
   (void)p;
}

static void tc_reset( const clap_plugin_t *p )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;
   /* The effects are stateless DSP; the instrument and the arp are not, and
    * their reset must be TOTAL for the determinism gate to mean anything. */
   tc_voices_reset( self );
   self->nHeld         = 0;
   self->arpFrame      = 0;
   self->arpOffPending = 0;
   self->arpOffAt      = 0;
   self->arpNextId     = 0;
}

/* --- the effects: render [from, to) with the current parameters ----------- */

static void tc_render_effect( tw_testclap_t *self, const clap_audio_buffer_t *ib,
                              clap_audio_buffer_t *ob, uint32_t from, uint32_t to,
                              uint32_t framesCount )
{
   const int   report = self->report >= 0.5;
   const float g      = (float)self->gain;
   const float th     = (float)self->clip;

   for( uint32_t c = 0; c < ob->channel_count; ++c ) {
      float *o = ob->data32[c];
      if( !o )
         continue;
      if( report ) {
         for( uint32_t i = from; i < to; ++i )
            o[i] = (float)framesCount;
         continue;
      }
      const float *in = ( c < ib->channel_count ) ? ib->data32[c] : NULL;
      if( !in ) {
         memset( o + from, 0, (size_t)( to - from ) * sizeof( float ) );
         continue;
      }
      if( !self->kind || self->kind == TW_KIND_GAIN ) {
         for( uint32_t i = from; i < to; ++i )
            o[i] = in[i] * g;
      } else if( c == 0 && ib->channel_count > 1 && ib->data32[1] ) {
         /* The cross-channel term (see the header comment): channel 0 mixes in
          * HALF of itself plus ALL of channel 1, every other channel is halved. */
         const float *in1 = ib->data32[1];
         for( uint32_t i = from; i < to; ++i )
            o[i] = ( in[i] * 0.5f + in1[i] ) * g;
      } else {
         for( uint32_t i = from; i < to; ++i )
            o[i] = in[i] * 0.5f * g;
      }

      /* The clipper runs AFTER the gain — which is the whole point of it. */
      if( th > 0.0f ) {
         for( uint32_t i = from; i < to; ++i ) {
            if( o[i] > th )       o[i] = th;
            else if( o[i] < -th ) o[i] = -th;
         }
      }
   }
}

static clap_process_status tc_process_effect( tw_testclap_t *self,
                                              const clap_process_t *proc )
{
   if( !proc->audio_inputs || !proc->audio_outputs )
      return CLAP_PROCESS_ERROR;
   if( proc->audio_inputs_count < 1 || proc->audio_outputs_count < 1 )
      return CLAP_PROCESS_ERROR;

   const clap_audio_buffer_t *ib = &proc->audio_inputs[0];
   clap_audio_buffer_t       *ob = &proc->audio_outputs[0];
   if( !ib->data32 || !ob->data32 )
      return CLAP_PROCESS_ERROR;

   const uint32_t n  = proc->frames_count;
   const clap_input_events_t *in = proc->in_events;
   const uint32_t nev = ( in && in->size && in->get ) ? in->size( in ) : 0;

   /* Render in SEGMENTS split at each parameter event, so a value applies from
    * exactly its own frame (proposal 37 AC2). With no event, or with every event
    * at time 0 (the pre-36 parameter ring), this is one segment and exactly the
    * arithmetic M1 had — which is why the effect goldens do not move. */
   uint32_t pos = 0;
   for( uint32_t i = 0; i < nev; ++i ) {
      const clap_event_header_t *h = in->get( in, i );
      if( !h || h->space_id != CLAP_CORE_EVENT_SPACE_ID )
         continue;
      if( h->type != CLAP_EVENT_PARAM_VALUE )
         continue;
      uint32_t t = h->time < n ? h->time : n;
      if( t > pos ) {
         tc_render_effect( self, ib, ob, pos, t, n );
         pos = t;
      }
      const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
      tc_apply_param( self, e->param_id, e->value );
   }
   if( pos < n )
      tc_render_effect( self, ib, ob, pos, n, n );

   return CLAP_PROCESS_CONTINUE;
}

/* --- the instrument ------------------------------------------------------- */

static void tc_render_sine( tw_testclap_t *self, clap_audio_buffer_t *main,
                            clap_audio_buffer_t *aux, uint32_t from, uint32_t to )
{
   const float g = (float)self->gain;
   /* Channels 1.. of the MAIN out at half amplitude when the skew is on. Closed
    * form on purpose: the level relation between the channels is exactly 2, at
    * every sample, so a file assertion measures a ratio and not a texture. */
   const float r = self->skew >= 0.5 ? 0.5f : 1.0f;
   for( uint32_t i = from; i < to; ++i ) {
      double sum = 0.0;
      for( int vi = 0; vi < TW_SINE_VOICES; ++vi ) {
         tw_voice_t *v = &self->voices[vi];
         if( !v->active )
            continue;
         sum += v->velocity * sin( 2.0 * TW_PI * v->phase );
         v->phase += v->phaseInc;
         if( v->phase >= 1.0 )
            v->phase -= floor( v->phase );
      }
      const float s = (float)sum * g;
      if( main && main->data32 )
         for( uint32_t c = 0; c < main->channel_count; ++c )
            if( main->data32[c] ) main->data32[c][i] = c == 0 ? s : s * r;
      /* The aux carries the same signal at -6 dB, so a multi-out gate can tell
       * the two buses apart without needing two different notes. */
      if( aux && aux->data32 && aux->channel_count > 0 && aux->data32[0] )
         aux->data32[0][i] = s * 0.5f;
   }
}

static clap_process_status tc_process_sine( tw_testclap_t *self,
                                            const clap_process_t *proc )
{
   if( !proc->audio_outputs || proc->audio_outputs_count < 1 )
      return CLAP_PROCESS_ERROR;

   clap_audio_buffer_t *main = &proc->audio_outputs[0];
   clap_audio_buffer_t *aux  = proc->audio_outputs_count > 1
                                   ? &proc->audio_outputs[1] : NULL;

   const uint32_t n  = proc->frames_count;
   const clap_input_events_t  *in  = proc->in_events;
   const clap_output_events_t *out = proc->out_events;
   const uint32_t nev = ( in && in->size && in->get ) ? in->size( in ) : 0;

   uint32_t pos = 0;
   for( uint32_t i = 0; i < nev; ++i ) {
      const clap_event_header_t *h = in->get( in, i );
      if( !h || h->space_id != CLAP_CORE_EVENT_SPACE_ID )
         continue;
      if( h->type != CLAP_EVENT_NOTE_ON && h->type != CLAP_EVENT_NOTE_OFF &&
          h->type != CLAP_EVENT_NOTE_CHOKE && h->type != CLAP_EVENT_PARAM_VALUE )
         continue;

      uint32_t t = h->time < n ? h->time : n;
      if( t > pos ) {
         tc_render_sine( self, main, aux, pos, t );
         pos = t;
      }

      if( h->type == CLAP_EVENT_PARAM_VALUE ) {
         const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
         tc_apply_param( self, e->param_id, e->value );
         continue;
      }

      const clap_event_note_t *ne = (const clap_event_note_t *)h;
      if( h->type == CLAP_EVENT_NOTE_ON ) {
         /* A wildcard note-on has lost the note's identity; a host that
          * forwards one to an instrument is broken, and failing loudly beats
          * inventing a pitch. */
         if( ne->key < 0 )
            return CLAP_PROCESS_ERROR;
         tc_note_on( self, ne );
      } else {
         tc_note_off( self, ne, out, t );
      }
   }
   if( pos < n )
      tc_render_sine( self, main, aux, pos, n );

   return CLAP_PROCESS_CONTINUE;
}

/* --- the arpeggiator ------------------------------------------------------ */

static void tc_arp_push( const clap_output_events_t *out, uint16_t type, uint32_t time,
                         int key, int16_t chan, int32_t noteId )
{
   if( !out || !out->try_push )
      return;
   clap_event_note_t e;
   memset( &e, 0, sizeof( e ) );
   e.header.size     = sizeof( e );
   e.header.time     = time;
   e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
   e.header.type     = type;
   e.note_id         = noteId;
   e.port_index      = 0;
   e.channel         = chan;
   e.key             = (int16_t)key;
   e.velocity        = type == CLAP_EVENT_NOTE_ON ? 0.8 : 0.0;
   out->try_push( out, &e.header );
}

static void tc_arp_hold( tw_testclap_t *self, int key, int16_t chan )
{
   for( int i = 0; i < self->nHeld; ++i )
      if( self->heldKeys[i] == key )
         return;
   if( self->nHeld >= TW_SINE_VOICES )
      return;
   self->heldKeys[self->nHeld]  = key;
   self->heldChans[self->nHeld] = chan;
   ++self->nHeld;
}

static void tc_arp_release( tw_testclap_t *self, int key )
{
   for( int i = 0; i < self->nHeld; ++i ) {
      if( self->heldKeys[i] != key && key >= 0 )
         continue;
      for( int j = i; j + 1 < self->nHeld; ++j ) {
         self->heldKeys[j]  = self->heldKeys[j + 1];
         self->heldChans[j] = self->heldChans[j + 1];
      }
      --self->nHeld;
      return;
   }
}

static clap_process_status tc_process_arp( tw_testclap_t *self,
                                           const clap_process_t *proc )
{
   const uint32_t n = proc->frames_count;
   const clap_input_events_t  *in  = proc->in_events;
   const clap_output_events_t *out = proc->out_events;
   const uint32_t nev = ( in && in->size && in->get ) ? in->size( in ) : 0;

   uint32_t ev = 0;
   for( uint32_t i = 0; i < n; ++i ) {
      /* Apply every input event whose frame has arrived, BEFORE the grid check
       * for that frame — so a key pressed at offset 0 of a block that starts on
       * a grid boundary produces a note there, deterministically. */
      while( ev < nev ) {
         const clap_event_header_t *h = in->get( in, ev );
         if( !h ) { ++ev; continue; }
         if( h->time > i )
            break;
         ++ev;
         if( h->space_id != CLAP_CORE_EVENT_SPACE_ID )
            continue;
         if( h->type == CLAP_EVENT_NOTE_ON ) {
            const clap_event_note_t *ne = (const clap_event_note_t *)h;
            tc_arp_hold( self, ne->key, ne->channel );
         } else if( h->type == CLAP_EVENT_NOTE_OFF ) {
            const clap_event_note_t *ne = (const clap_event_note_t *)h;
            tc_arp_release( self, ne->key );
         }
      }

      /* The scheduled gate close. */
      if( self->arpOffPending && self->arpFrame == self->arpOffAt ) {
         tc_arp_push( out, CLAP_EVENT_NOTE_OFF, i, self->arpOffKey,
                      self->arpOffChan, self->arpOffId );
         self->arpOffPending = 0;
      }

      /* The grid. Absolute frames since reset, so the emission pattern does not
       * depend on how the host chunks the block. */
      if( ( self->arpFrame % TW_ARP_GRID ) == 0 && self->nHeld > 0 &&
          !self->arpOffPending ) {
         const int     step = (int)( ( self->arpFrame / TW_ARP_GRID ) % (uint64_t)self->nHeld );
         const int     key  = self->heldKeys[step];
         const int16_t chan = self->heldChans[step];
         const int32_t id   = self->arpNextId++;
         tc_arp_push( out, CLAP_EVENT_NOTE_ON, i, key, chan, id );
         self->arpOffPending = 1;
         self->arpOffAt      = self->arpFrame + TW_ARP_GATE;
         self->arpOffKey     = key;
         self->arpOffChan    = chan;
         self->arpOffId      = id;
      }

      ++self->arpFrame;
   }

   /* Anything the host placed past the end of the block still belongs here. */
   for( ; ev < nev; ++ev ) {
      const clap_event_header_t *h = in->get( in, ev );
      if( !h || h->space_id != CLAP_CORE_EVENT_SPACE_ID )
         continue;
      if( h->type == CLAP_EVENT_NOTE_ON )
         tc_arp_hold( self, ( (const clap_event_note_t *)h )->key,
                      ( (const clap_event_note_t *)h )->channel );
      else if( h->type == CLAP_EVENT_NOTE_OFF )
         tc_arp_release( self, ( (const clap_event_note_t *)h )->key );
   }

   return CLAP_PROCESS_CONTINUE;
}

static clap_process_status tc_process( const clap_plugin_t *p, const clap_process_t *proc )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;

   if( !proc )
      return CLAP_PROCESS_ERROR;
   /* The whole point of the fixture: a host that forgets to chunk is caught. */
   if( proc->frames_count > self->maxFrames )
      return CLAP_PROCESS_ERROR;

   /* The second route a GUI edit can leave by. Nothing is emitted unless a
    * gesture is queued, so the effect kinds are bit-for-bit unchanged. */
   tc_gui_emit_pending( self, proc->out_events );

   if( self->kind == TW_KIND_SINE )
      return tc_process_sine( self, proc );
   if( self->kind == TW_KIND_ARP )
      return tc_process_arp( self, proc );
   return tc_process_effect( self, proc );
}

/* ------------------------------------------------------------------- gui */

/* The api this build embeds with. The fixture claims exactly one, so a host
 * that offers a handle tagged for a different platform is refused rather than
 * silently accepted. */
#if defined( _WIN32 )
#define TW_GUI_API CLAP_WINDOW_API_WIN32
#elif defined( __APPLE__ )
#define TW_GUI_API CLAP_WINDOW_API_COCOA
#else
#define TW_GUI_API CLAP_WINDOW_API_X11
#endif

static bool tc_gui_is_api_supported( const clap_plugin_t *p, const char *api,
                                     bool is_floating )
{
   (void)p;
   /* Both forms, so the D1 floating rung has something to exercise. */
   (void)is_floating;
   return api && !strcmp( api, TW_GUI_API );
}

static bool tc_gui_get_preferred_api( const clap_plugin_t *p, const char **api,
                                      bool *is_floating )
{
   (void)p;
   if( !api || !is_floating )
      return false;
   *api         = TW_GUI_API;
   *is_floating = false;
   return true;
}

static bool tc_gui_create( const clap_plugin_t *p, const char *api, bool is_floating )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;
   if( !self->hasGui || self->guiCreated )
      return false;
   if( !api || strcmp( api, TW_GUI_API ) )
      return false;
   self->guiCreated  = 1;
   self->guiFloating = is_floating ? 1 : 0;
   self->guiParented = 0;
   self->guiShown    = 0;
   self->guiW        = 320;
   self->guiH        = 240;
   return true;
}

static void tc_gui_destroy( const clap_plugin_t *p )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;
   self->guiCreated = 0;
   self->guiShown   = 0;
}

static bool tc_gui_set_scale( const clap_plugin_t *p, double scale )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;
   self->guiScale = scale;
   return true;
}

static bool tc_gui_get_size( const clap_plugin_t *p, uint32_t *w, uint32_t *h )
{
   const tw_testclap_t *self = (const tw_testclap_t *)p->plugin_data;
   if( !self->guiCreated || !w || !h )
      return false;
   *w = self->guiW;
   *h = self->guiH;
   return true;
}

static bool tc_gui_can_resize( const clap_plugin_t *p )
{
   (void)p;
   return true;
}

static bool tc_gui_get_resize_hints( const clap_plugin_t *p, clap_gui_resize_hints_t *hints )
{
   (void)p;
   (void)hints;
   return false;
}

/* Rounds DOWN to a multiple of 16, minimum 16. A closed form, so a host test
 * can tell "constrain() asked the plugin" from "constrain() passed the value
 * through". */
static bool tc_gui_adjust_size( const clap_plugin_t *p, uint32_t *w, uint32_t *h )
{
   (void)p;
   if( !w || !h )
      return false;
   *w = ( *w / 16 ) * 16;
   *h = ( *h / 16 ) * 16;
   if( *w < 16 ) *w = 16;
   if( *h < 16 ) *h = 16;
   return true;
}

static bool tc_gui_set_size( const clap_plugin_t *p, uint32_t w, uint32_t h )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;
   if( !self->guiCreated || self->guiFloating )
      return false;
   self->guiW = w;
   self->guiH = h;
   /* A knob move with NO request_flush: it can only reach the host through
    * process(), which is the second of the two routes M6 has to cover. */
   self->guiPendingEdit  = 1;
   self->guiPendingValue = 0.75;
   return true;
}

static bool tc_gui_set_parent( const clap_plugin_t *p, const clap_window_t *win )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;
   if( !self->guiCreated || self->guiFloating || !win )
      return false;
   if( !win->api || strcmp( win->api, TW_GUI_API ) )
      return false;
   /* No SetParent, no XReparentWindow: there is no window. Recording that the
    * host got this far is the assertion. */
   self->guiParented = 1;
   return true;
}

static bool tc_gui_set_transient( const clap_plugin_t *p, const clap_window_t *win )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;
   if( !self->guiCreated || !self->guiFloating )
      return false;
   self->guiParented = win ? 1 : 0;
   return true;
}

static void tc_gui_suggest_title( const clap_plugin_t *p, const char *title )
{
   (void)p;
   (void)title;
}

static bool tc_gui_show( const clap_plugin_t *p )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;
   if( !self->guiCreated )
      return false;
   self->guiShown = 1;

   /* Stand in for the user turning the Gain knob. The value is applied
    * INTERNALLY first, exactly as a real plugin does - its DSP follows its own
    * GUI without asking the host - and only then offered to the host. */
   self->gain            = 2.5;
   self->guiPendingEdit  = 1;
   self->guiPendingValue = 2.5;

   if( self->host && self->host->get_extension ) {
      const clap_host_params_t *hp =
         (const clap_host_params_t *)self->host->get_extension( self->host, CLAP_EXT_PARAMS );
      if( hp && hp->request_flush )
         hp->request_flush( self->host );

      const clap_host_gui_t *hg =
         (const clap_host_gui_t *)self->host->get_extension( self->host, CLAP_EXT_GUI );
      if( hg && hg->request_resize )
         hg->request_resize( self->host, 300, 200 );
   }
   return true;
}

static bool tc_gui_hide( const clap_plugin_t *p )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;
   self->guiShown = 0;
   return true;
}

static const clap_plugin_gui_t s_gui = {
   .is_api_supported  = tc_gui_is_api_supported,
   .get_preferred_api = tc_gui_get_preferred_api,
   .create            = tc_gui_create,
   .destroy           = tc_gui_destroy,
   .set_scale         = tc_gui_set_scale,
   .get_size          = tc_gui_get_size,
   .can_resize        = tc_gui_can_resize,
   .get_resize_hints  = tc_gui_get_resize_hints,
   .adjust_size       = tc_gui_adjust_size,
   .set_size          = tc_gui_set_size,
   .set_parent        = tc_gui_set_parent,
   .set_transient     = tc_gui_set_transient,
   .suggest_title     = tc_gui_suggest_title,
   .show              = tc_gui_show,
   .hide              = tc_gui_hide,
};

static const void *tc_get_extension( const clap_plugin_t *p, const char *id )
{
   const tw_testclap_t *self = (const tw_testclap_t *)p->plugin_data;
   if( !strcmp( id, CLAP_EXT_GUI ) && self->hasGui )                      return &s_gui;
   if( !strcmp( id, CLAP_EXT_AUDIO_PORTS ) && self->kind != TW_KIND_ARP ) return &s_ports;
   if( !strcmp( id, CLAP_EXT_PARAMS ) && self->kind != TW_KIND_ARP )      return &s_params;
   if( !strcmp( id, CLAP_EXT_STATE ) && self->kind != TW_KIND_ARP )       return &s_state;
   if( !strcmp( id, CLAP_EXT_LATENCY ) )     return &s_latency;
   if( !strcmp( id, CLAP_EXT_TAIL ) )        return &s_tail;
   if( !strcmp( id, CLAP_EXT_NOTE_PORTS ) &&
       ( self->kind == TW_KIND_SINE || self->kind == TW_KIND_ARP ) )
      return &s_note_ports;
   return NULL;
}

static void tc_on_main_thread( const clap_plugin_t *p )
{
   (void)p;
}

/* --------------------------------------------------------------- factory */

static uint32_t tc_factory_count( const clap_plugin_factory_t *f )
{
   (void)f;
   return 5;
}

static const clap_plugin_descriptor_t *
tc_factory_descriptor( const clap_plugin_factory_t *f, uint32_t index )
{
   (void)f;
   if( index == 0 ) return &s_desc;
   if( index == 1 ) return &s_desc_skew;
   if( index == 2 ) return &s_desc_sine;
   if( index == 3 ) return &s_desc_arp;
   if( index == 4 ) return &s_desc_gui;
   return NULL;
}

static const clap_plugin_t *tc_factory_create( const clap_plugin_factory_t *f,
                                               const clap_host_t          *host,
                                               const char                 *id )
{
   (void)f;
   if( !id )
      return NULL;

   int kind;
   const clap_plugin_descriptor_t *desc;
   if( !strcmp( id, TW_TESTCLAP_ID ) )           { kind = TW_KIND_GAIN; desc = &s_desc; }
   else if( !strcmp( id, TW_TESTCLAP_SKEW_ID ) ) { kind = TW_KIND_SKEW; desc = &s_desc_skew; }
   else if( !strcmp( id, TW_TESTCLAP_SINE_ID ) ) { kind = TW_KIND_SINE; desc = &s_desc_sine; }
   else if( !strcmp( id, TW_TESTCLAP_ARP_ID ) )  { kind = TW_KIND_ARP;  desc = &s_desc_arp; }
   /* GUI behaves as GAIN in every respect that is not the gui extension - same
    * params, same ports, same DSP - so it reuses the kind rather than adding a
    * fifth branch to every switch in the file. hasGui is the only difference. */
   else if( !strcmp( id, TW_TESTCLAP_GUI_ID ) )  { kind = TW_KIND_GAIN; desc = &s_desc_gui; }
   else                                          { return NULL; }

   tw_testclap_t *self = (tw_testclap_t *)calloc( 1, sizeof( tw_testclap_t ) );
   if( !self )
      return NULL;

   self->host       = host;
   self->kind       = kind;
   self->gain       = 1.0;
   self->report     = 0.0;
   self->clip       = 0.0;
   self->skew       = 0.0;
   self->maxFrames  = 0;
   self->sampleRate = 48000.0;
   self->active     = 0;
   self->hasGui     = !strcmp( id, TW_TESTCLAP_GUI_ID );
   self->guiScale   = 1.0;

   self->base.desc            = desc;
   self->base.plugin_data     = self;
   self->base.init            = tc_init;
   self->base.destroy         = tc_destroy;
   self->base.activate        = tc_activate;
   self->base.deactivate      = tc_deactivate;
   self->base.start_processing = tc_start_processing;
   self->base.stop_processing  = tc_stop_processing;
   self->base.reset            = tc_reset;
   self->base.process          = tc_process;
   self->base.get_extension    = tc_get_extension;
   self->base.on_main_thread   = tc_on_main_thread;

   return &self->base;
}

static const clap_plugin_factory_t s_factory = {
   .get_plugin_count      = tc_factory_count,
   .get_plugin_descriptor = tc_factory_descriptor,
   .create_plugin         = tc_factory_create,
};

/* ----------------------------------------------------------------- entry */

static bool tc_entry_init( const char *path )
{
   (void)path;
   return true;
}

static void tc_entry_deinit( void )
{
}

static const void *tc_entry_get_factory( const char *id )
{
   if( id && !strcmp( id, CLAP_PLUGIN_FACTORY_ID ) )
      return &s_factory;
   return NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
   .clap_version = CLAP_VERSION_INIT,
   .init         = tc_entry_init,
   .deinit       = tc_entry_deinit,
   .get_factory  = tc_entry_get_factory,
};
