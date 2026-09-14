// The ONLY place with engine-version preprocessor switches.
// Everything else goes through IUnrealAdapter.
#pragma once

#include "CoreMinimal.h"
#include "Misc/EngineVersionComparison.h"
#include "HttpRequestHandler.h"

#define CLAUDE_AGENT_UE_AT_LEAST(Major, Minor) (!UE_VERSION_OLDER_THAN(Major, Minor, 0))

// UE 5.4 changed FHttpRequestHandler from a TFunction to a TDelegate.
#if CLAUDE_AGENT_UE_AT_LEAST(5, 4)
	#define CLAUDE_AGENT_MAKE_HTTP_HANDLER(Lambda) FHttpRequestHandler::CreateLambda(Lambda)
#else
	#define CLAUDE_AGENT_MAKE_HTTP_HANDLER(Lambda) FHttpRequestHandler(Lambda)
#endif

// Engine minor version, as an int, for adapter selection at runtime.
#define CLAUDE_AGENT_ENGINE_MAJOR ENGINE_MAJOR_VERSION
#define CLAUDE_AGENT_ENGINE_MINOR ENGINE_MINOR_VERSION
