// Minimal find_package(haze) consumer. Proves the installed package exports
// haze::haze with an include path carrying both haze.h and replay_bridge.h,
// and that the resulting binary links and runs against the shipped
// libhaze.so. Not a functional test of the FHE record/replay pipeline (see
// haze_tests for that); this only exercises the install/package boundary.

#include <haze/haze.h>
#include <haze/haze_types.h>
#include <haze/replay_bridge.h>
#include <stdio.h>

int main(void) {
    const char *message = hazeGetErrorString(HAZE_SUCCESS);
    if (message == NULL) {
        fprintf(stderr, "hazeGetErrorString returned NULL\n");
        return 1;
    }
    printf("haze package consumer: hazeGetErrorString(HAZE_SUCCESS) = \"%s\"\n", message);

    // References a replay_bridge.h symbol too, so this also proves that
    // header (installed separately from haze.h) resolves and links.
    hazeReplayBridgeReset();
    return 0;
}
