#pragma once
// Compile-time coherence guards. Included last.
#if defined(FEAT_PAD_EDITOR) && (CROSSPAD_PAD_CONFIG_LEVEL != CROSSPAD_PAD_CONFIG_FULL)
// Not an error: the editor is simply compiled out at GLOBAL level. This guard
// documents intent; the editor's own #if enforces it. No #error here.
#endif

#if CROSSPAD_PAD_CONFIG_LEVEL != CROSSPAD_PAD_CONFIG_FULL && \
    CROSSPAD_PAD_CONFIG_LEVEL != CROSSPAD_PAD_CONFIG_GLOBAL
#  error "CROSSPAD_PAD_CONFIG_LEVEL must be FULL(2) or GLOBAL(1)"
#endif
