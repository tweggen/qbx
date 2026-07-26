/* twtestclap — an in-repo CLAP plugin used as a BUILD-TIME TEST FIXTURE.
 *
 * proposal 08 M1. Without this, the only way to exercise the real CLAP load
 * path (LoadLibrary/dlopen -> clap_entry -> factory -> activate -> process ->
 * params -> state) is to install a third-party plugin on the machine running
 * the tests, which no CI and no fresh checkout can be asked to do. This module
 * is built as a MODULE library named twtestclap.clap and its absolute path is
 * handed to plugins_test as a compile definition.
 *
 * It is written in C on purpose: a C DLL has no libstdc++/libgcc import, so it
 * loads regardless of where the test binary runs from or what is on PATH.
 *
 * Behaviour — 2 in / 2 out, two parameters:
 *   id 0  "Gain"              0 .. 4, default 1      out = in * gain
 *   id 1  "Report Block Size" 0 .. 1, default 0, stepped
 *                             when >= 0.5, every output sample is set to
 *                             (float)frames_count instead of the gain result.
 *                             That is how the host-side chunking test observes
 *                             the block size the plugin actually saw.
 *
 * It also implements clap.state (the two parameter values, little-endian
 * doubles) and clap.latency (a fixed 0), so the host's state framing and
 * latency query have something real to talk to.
 *
 * Deliberate strictness: if process() is ever handed more frames than the
 * host promised at activate(), the plugin returns CLAP_PROCESS_ERROR instead of
 * overrunning. A host-side chunking regression therefore fails loudly.
 */

#include <clap/clap.h>

#include <stdlib.h>
#include <string.h>

#define TW_TESTCLAP_ID "tw.test.clap.gain"

static const char *const s_features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
                                         CLAP_PLUGIN_FEATURE_UTILITY,
                                         CLAP_PLUGIN_FEATURE_STEREO,
                                         NULL };

static const clap_plugin_descriptor_t s_desc = {
   .clap_version = CLAP_VERSION_INIT,
   .id           = TW_TESTCLAP_ID,
   .name         = "Smaragd Test Gain",
   .vendor       = "Smaragd",
   .url          = "https://github.com/tweggen/qbx",
   .manual_url   = "",
   .support_url  = "",
   .version      = "1.0.0",
   .description  = "Test fixture: stereo gain with a block-size reporter",
   .features     = s_features,
};

typedef struct {
   clap_plugin_t      base;
   const clap_host_t *host;

   double   gain;
   double   report;
   uint32_t maxFrames;
   int      active;
} tw_testclap_t;

/* ---------------------------------------------------------------- params */

static uint32_t tc_params_count( const clap_plugin_t *p )
{
   (void)p;
   return 2;
}

static bool tc_params_get_info( const clap_plugin_t *p, uint32_t index,
                                clap_param_info_t *info )
{
   (void)p;
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
   if( index == 1 ) {
      info->id            = 1;
      info->flags         = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
      info->min_value     = 0.0;
      info->max_value     = 1.0;
      info->default_value = 0.0;
      strncpy( info->name, "Report Block Size", CLAP_NAME_SIZE - 1 );
      return true;
   }
   return false;
}

static bool tc_params_get_value( const clap_plugin_t *p, clap_id id, double *out )
{
   const tw_testclap_t *self = (const tw_testclap_t *)p->plugin_data;
   if( id == 0 ) { *out = self->gain;   return true; }
   if( id == 1 ) { *out = self->report; return true; }
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
      if( e->param_id == 0 ) self->gain   = e->value;
      if( e->param_id == 1 ) self->report = e->value;
   }
}

static void tc_params_flush( const clap_plugin_t *p, const clap_input_events_t *in,
                             const clap_output_events_t *out )
{
   (void)out;
   tc_apply_events( (tw_testclap_t *)p->plugin_data, in );
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
   (void)p; (void)is_input;
   return 1;
}

static bool tc_ports_get( const clap_plugin_t *p, uint32_t index, bool is_input,
                          clap_audio_port_info_t *info )
{
   (void)p;
   if( index != 0 )
      return false;
   memset( info, 0, sizeof( *info ) );
   info->id            = is_input ? 0 : 1;
   info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
   info->channel_count = 2;
   info->port_type     = CLAP_PORT_STEREO;
   info->in_place_pair = CLAP_INVALID_ID;
   strncpy( info->name, is_input ? "Main In" : "Main Out", CLAP_NAME_SIZE - 1 );
   return true;
}

static const clap_plugin_audio_ports_t s_ports = {
   .count = tc_ports_count,
   .get   = tc_ports_get,
};

/* ----------------------------------------------------------------- state */

static void tc_put_double( uint8_t *p, double v )
{
   /* All targets we build for are little-endian; memcpy keeps it aligned-safe. */
   memcpy( p, &v, sizeof( double ) );
}

static bool tc_state_save( const clap_plugin_t *p, const clap_ostream_t *os )
{
   const tw_testclap_t *self = (const tw_testclap_t *)p->plugin_data;
   uint8_t buf[16];
   tc_put_double( buf,     self->gain );
   tc_put_double( buf + 8, self->report );

   uint64_t written = 0;
   while( written < sizeof( buf ) ) {
      int64_t n = os->write( os, buf + written, sizeof( buf ) - written );
      if( n <= 0 )
         return false;
      written += (uint64_t)n;
   }
   return true;
}

static bool tc_state_load( const clap_plugin_t *p, const clap_istream_t *is )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;
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
   (void)sample_rate;
   (void)min_frames;
   if( max_frames == 0 )
      return false;
   self->maxFrames = max_frames;
   self->active    = 1;
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
   (void)p;   /* stateless DSP */
}

static clap_process_status tc_process( const clap_plugin_t *p, const clap_process_t *proc )
{
   tw_testclap_t *self = (tw_testclap_t *)p->plugin_data;

   if( !proc || !proc->audio_inputs || !proc->audio_outputs )
      return CLAP_PROCESS_ERROR;
   if( proc->audio_inputs_count < 1 || proc->audio_outputs_count < 1 )
      return CLAP_PROCESS_ERROR;
   /* The whole point of the fixture: a host that forgets to chunk is caught. */
   if( proc->frames_count > self->maxFrames )
      return CLAP_PROCESS_ERROR;

   tc_apply_events( self, proc->in_events );

   const clap_audio_buffer_t *ib = &proc->audio_inputs[0];
   clap_audio_buffer_t       *ob = &proc->audio_outputs[0];
   if( !ib->data32 || !ob->data32 )
      return CLAP_PROCESS_ERROR;

   const uint32_t n     = proc->frames_count;
   const int      report = self->report >= 0.5;
   const float    g      = (float)self->gain;

   for( uint32_t c = 0; c < ob->channel_count; ++c ) {
      float *o = ob->data32[c];
      if( !o )
         continue;
      if( report ) {
         for( uint32_t i = 0; i < n; ++i )
            o[i] = (float)n;
         continue;
      }
      const float *in = ( c < ib->channel_count ) ? ib->data32[c] : NULL;
      if( in ) {
         for( uint32_t i = 0; i < n; ++i )
            o[i] = in[i] * g;
      } else {
         memset( o, 0, (size_t)n * sizeof( float ) );
      }
   }
   return CLAP_PROCESS_CONTINUE;
}

static const void *tc_get_extension( const clap_plugin_t *p, const char *id )
{
   (void)p;
   if( !strcmp( id, CLAP_EXT_AUDIO_PORTS ) ) return &s_ports;
   if( !strcmp( id, CLAP_EXT_PARAMS ) )      return &s_params;
   if( !strcmp( id, CLAP_EXT_STATE ) )       return &s_state;
   if( !strcmp( id, CLAP_EXT_LATENCY ) )     return &s_latency;
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
   return 1;
}

static const clap_plugin_descriptor_t *
tc_factory_descriptor( const clap_plugin_factory_t *f, uint32_t index )
{
   (void)f;
   return index == 0 ? &s_desc : NULL;
}

static const clap_plugin_t *tc_factory_create( const clap_plugin_factory_t *f,
                                               const clap_host_t          *host,
                                               const char                 *id )
{
   (void)f;
   if( !id || strcmp( id, TW_TESTCLAP_ID ) )
      return NULL;

   tw_testclap_t *self = (tw_testclap_t *)calloc( 1, sizeof( tw_testclap_t ) );
   if( !self )
      return NULL;

   self->host      = host;
   self->gain      = 1.0;
   self->report    = 0.0;
   self->maxFrames = 0;
   self->active    = 0;

   self->base.desc            = &s_desc;
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
