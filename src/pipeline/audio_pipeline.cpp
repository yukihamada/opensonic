#include <soluna/pipeline/pipeline.h>
#include <soluna/pipeline/ring_buffer.h>
#include <soluna/pipeline/dsp_plugin.h>

namespace soluna::pipeline {

// The audio pipeline is assembled directly in the daemon main().
// DSP plugins are inserted into the pipeline via PluginHost::process_all(),
// which is called between the ring buffer read and the network send (TX)
// or between the network receive and the ring buffer write (RX).
//
// Pipeline flow:
//   TX: Audio Input → Ring Buffer → [DSP Plugins] → RTP Send
//   RX: RTP Recv → [DSP Plugins] → Ring Buffer → Audio Output

} // namespace soluna::pipeline
