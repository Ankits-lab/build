################################################
## A thin wrapper around BUILD_HOST_EXECUTABLE
## Common flags for host fuzz tests are added.
################################################
$(call record-module-type,HOST_FUZZ_TEST)

LOCAL_SANITIZE += fuzzer
LOCAL_STATIC_LIBRARIES += libLLVMFuzzer

include $(BUILD_HOST_EXECUTABLE)
