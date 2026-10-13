/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef TLGGPU_H
#define TLGGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} TlgGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void tlggpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int tlggpu_init(const TlgGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t tlggpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int tlggpu_handle_fault(void *ucontext, void *address);
/* TLG_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void tlggpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int tlggpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int tlggpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int tlggpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int tlggpu_choice_poll(void);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int tlggpu_overlay_captures_input(void);
/* A short message at the top of the screen (any thread). */
void tlggpu_overlay_notice(const char *text, float seconds);
/* Guest memcpy (src) / memset (src NULL, fill) into GPU-modified memory without a fault or a
 * GPU wait (TLG_WRITE_MERGE=1); 0: the caller writes normally. */
int tlggpu_cpu_write(void *dst, const void *src, int fill, uint64_t size);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned tlggpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
