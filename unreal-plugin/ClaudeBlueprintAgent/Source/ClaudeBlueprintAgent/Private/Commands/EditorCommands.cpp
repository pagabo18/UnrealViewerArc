// editor.* commands: viewport screenshots (experimental).
#include "CommandHelpers.h"
#include "Core/AgentCommandRegistry.h"
#include "Core/AgentConfig.h"
#include "Editor.h"
#include "UnrealClient.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

namespace
{
	FAgentResult Cmd_Screenshot(const FJsonObject& Params, FAgentContext& Context)
	{
		if (!GEditor)
		{
			return FAgentResult::Error(AgentErrors::Internal, TEXT("No editor."));
		}
		FString Path = AgentJson::GetString(Params, TEXT("path"));
		if (Path.IsEmpty())
		{
			const FString Dir = FPaths::Combine(FAgentConfig::GetSavedDir(), TEXT("Screenshots"));
			IFileManager::Get().MakeDirectory(*Dir, true);
			Path = FPaths::Combine(Dir, FString::Printf(TEXT("Viewport_%s.png"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
		}
		// Asynchronous: the engine writes the file at the end of the next frame.
		FScreenshotRequest::RequestScreenshot(Path, AgentJson::GetBool(Params, TEXT("ui"), true), /*bAddFilenameSuffix*/ false);
		TSharedRef<FJsonObject> Json = AgentJson::Obj();
		Json->SetStringField(TEXT("image"), FPaths::ConvertRelativePathToFull(Path));
		Json->SetBoolField(TEXT("async"), true);
		Json->SetStringField(TEXT("note"), TEXT("Written after the next editor frame; the editor window must be visible (not minimized)."));
		return FAgentResult::Ok(Json);
	}
}

void RegisterEditorCommands(FAgentCommandRegistry& Registry)
{
	Registry.Register(TEXT("editor.screenshot"), TEXT("Request an editor viewport screenshot (async, experimental)."), false, &Cmd_Screenshot);
}
