#pragma once
#include "assistant_profiles.h"
namespace valet {
// Accepts the canonical {url, code} reply (code contains url + bootstrapToken).
// Returns a user-visible error; never includes credentials in the error.
const char* ParsePairing(const std::string& payload, AssistantProfile& profile);
}
