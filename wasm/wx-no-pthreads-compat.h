#pragma once

// PCBJam/wxWidgets currently includes Emscripten's legacy pthread proxy API
// unconditionally in src/wasm/utils.cpp. NeoTools deliberately builds this
// port without pthreads, so all wxWidgets code runs on the runtime main thread.
//
// This header is force-included only while compiling wxWidgets. It leaves the
// upstream checkout unchanged and folds the two invalid pthread-only calls out
// of the no-pthreads build before code generation.
#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>

#undef emscripten_is_main_runtime_thread
#define emscripten_is_main_runtime_thread() (1)

#undef emscripten_async_run_in_main_runtime_thread
#define emscripten_async_run_in_main_runtime_thread(...) ((void)0)
#endif
