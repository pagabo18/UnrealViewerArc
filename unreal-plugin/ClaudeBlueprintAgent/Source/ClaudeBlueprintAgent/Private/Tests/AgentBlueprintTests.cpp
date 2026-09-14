// Editor automation tests: run with
//   UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests ClaudeBlueprintAgent" -unattended -nopause -testexit="Automation Test Queue Empty" -log
#include "AgentTestUtils.h"
#include "EditorAssetLibrary.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

void ClaudeAgentTest_DeleteAsset(const FString& PackagePath)
{
	if (UEditorAssetLibrary::DoesAssetExist(PackagePath))
	{
		UEditorAssetLibrary::DeleteAsset(PackagePath);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaudeAgentPingTest, "ClaudeBlueprintAgent.System.Ping", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FClaudeAgentPingTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Response = AgentTest::Exec(*this, TEXT("{'cmd':'system.ping'}"));
	TestTrue(TEXT("ping ok"), Response.IsValid() && AgentJson::GetBool(*Response, TEXT("ok")));
	TSharedPtr<FJsonObject> Caps = AgentTest::Result(AgentTest::Exec(*this, TEXT("{'cmd':'system.capabilities'}")));
	TestTrue(TEXT("capabilities present"), Caps.IsValid() && AgentJson::GetObject(*Caps, TEXT("capabilities")).IsValid());
	TSharedPtr<FJsonObject> Unknown = AgentTest::Exec(*this, TEXT("{'cmd':'nope.nothing'}"), false);
	TestTrue(TEXT("unknown command rejected"), Unknown.IsValid() && !AgentJson::GetBool(*Unknown, TEXT("ok")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaudeAgentBlueprintRoundtripTest, "ClaudeBlueprintAgent.Blueprint.Roundtrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FClaudeAgentBlueprintRoundtripTest::RunTest(const FString& Parameters)
{
	const FString Asset = FString(AgentTest::TestFolder) + TEXT("/BP_AgentRoundtrip");
	ClaudeAgentTest_DeleteAsset(Asset);

	// create
	TSharedPtr<FJsonObject> Created = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'assets.create_blueprint','params':{'path':'%s','parent':'Actor'}}"), *Asset)));
	TestEqual(TEXT("created asset path"), AgentTest::Str(Created, TEXT("asset")), Asset);

	// summary (small)
	TSharedPtr<FJsonObject> Summary = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'blueprint.summary','params':{'asset':'%s'}}"), *Asset)));
	TestEqual(TEXT("parent is Actor"), AgentTest::Str(Summary, TEXT("parent")), FString(TEXT("Actor")));
	TestEqual(TEXT("no variables yet"), AgentTest::Int(Summary, TEXT("variables")), 0);

	// variable add
	AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'blueprint.variable','params':{'asset':'%s','op':'add','name':'Stamina','type':'float','default':'100','category':'Stats'}}"), *Asset));
	TSharedPtr<FJsonObject> Structure = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'blueprint.structure','params':{'asset':'%s'}}"), *Asset)));
	const TArray<TSharedPtr<FJsonValue>>* Vars = Structure.IsValid() ? AgentJson::GetArray(*Structure, TEXT("variables")) : nullptr;
	TestTrue(TEXT("one variable"), Vars && Vars->Num() == 1);
	if (Vars && Vars->Num() == 1)
	{
		TestEqual(TEXT("variable name"), AgentJson::GetString(*(*Vars)[0]->AsObject(), TEXT("name")), FString(TEXT("Stamina")));
		TestEqual(TEXT("variable type"), AgentJson::GetString(*(*Vars)[0]->AsObject(), TEXT("type")), FString(TEXT("float")));
	}

	// undo removes the variable, redo restores it
	AgentTest::Exec(*this, TEXT("{'cmd':'system.undo','params':{'steps':1}}"));
	Summary = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'blueprint.summary','params':{'asset':'%s'}}"), *Asset)));
	TestEqual(TEXT("undo removed variable"), AgentTest::Int(Summary, TEXT("variables")), 0);
	AgentTest::Exec(*this, TEXT("{'cmd':'system.redo','params':{'steps':1}}"));
	Summary = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'blueprint.summary','params':{'asset':'%s'}}"), *Asset)));
	TestEqual(TEXT("redo restored variable"), AgentTest::Int(Summary, TEXT("variables")), 1);

	// function with signature
	TSharedPtr<FJsonObject> Function = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'blueprint.function','params':{'asset':'%s','op':'create','name':'AddStamina','inputs':['Amount:float'],'outputs':['NewValue:float']}}"), *Asset)));
	TestEqual(TEXT("function graph name"), AgentTest::Str(Function, TEXT("graph")), FString(TEXT("AddStamina")));
	const FString EntryGuid = AgentTest::Str(Function, TEXT("entry_node"));
	TestTrue(TEXT("entry node returned"), !EntryGuid.IsEmpty());

	// nodes: custom event -> print, connected via 'after'
	TSharedPtr<FJsonObject> Event = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'graph.add_node','params':{'asset':'%s','graph':'EventGraph','type':'custom_event','name':'OnAgentTest','inputs':['Value:int']}}"), *Asset)));
	const FString EventGuid = AgentTest::Str(Event, TEXT("guid"));
	TestTrue(TEXT("event created"), !EventGuid.IsEmpty());
	TSharedPtr<FJsonObject> Print = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'graph.add_node','params':{'asset':'%s','graph':'EventGraph','type':'print','text':'hello','after':'%s'}}"), *Asset, *EventGuid)));
	const FString PrintGuid = AgentTest::Str(Print, TEXT("guid"));
	const TArray<TSharedPtr<FJsonValue>>* Connections = Print.IsValid() ? AgentJson::GetArray(*Print, TEXT("connections")) : nullptr;
	TestTrue(TEXT("print connected after event"), Connections && Connections->Num() == 1);

	// windowed inspection around the event
	TSharedPtr<FJsonObject> Window = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'graph.inspect','params':{'asset':'%s','graph':'EventGraph','around':'%s','depth':1}}"), *Asset, *EventGuid)));
	const TArray<TSharedPtr<FJsonValue>>* Nodes = Window.IsValid() ? AgentJson::GetArray(*Window, TEXT("nodes")) : nullptr;
	TestTrue(TEXT("neighbourhood has event + print"), Nodes && Nodes->Num() == 2);

	// find nodes
	TSharedPtr<FJsonObject> Found = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'graph.find_nodes','params':{'asset':'%s','query':'PrintString'}}"), *Asset)));
	TestEqual(TEXT("found print node"), AgentTest::Int(Found, TEXT("matched")), 1);

	// set pin, replace, clone, delete
	AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'graph.set_pins','params':{'asset':'%s','node':'%s','pins':{'InString':'changed'}}}"), *Asset, *PrintGuid));
	TSharedPtr<FJsonObject> Cloned = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'graph.clone_nodes','params':{'asset':'%s','nodes':['%s']}}"), *Asset, *PrintGuid)));
	const TArray<TSharedPtr<FJsonValue>>* Clones = Cloned.IsValid() ? AgentJson::GetArray(*Cloned, TEXT("nodes")) : nullptr;
	TestTrue(TEXT("one clone"), Clones && Clones->Num() == 1);
	if (Clones && Clones->Num() == 1)
	{
		const FString CloneGuid = AgentJson::GetString(*(*Clones)[0]->AsObject(), TEXT("guid"));
		AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'graph.delete_nodes','params':{'asset':'%s','nodes':['%s']}}"), *Asset, *CloneGuid));
	}
	TSharedPtr<FJsonObject> Replaced = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'graph.replace_node','params':{'asset':'%s','node':'%s','type':'delay','duration':0.5}}"), *Asset, *PrintGuid)));
	TestEqual(TEXT("replacement is a call"), AgentTest::Str(Replaced, TEXT("kind")), FString(TEXT("CallFunction")));

	// compile + validate + save
	TSharedPtr<FJsonObject> Compile = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'blueprint.compile','params':{'assets':['%s'],'validate':true,'save':true}}"), *Asset)));
	TestEqual(TEXT("compile succeeded"), AgentTest::Int(Compile, TEXT("succeeded")), 1);
	const TArray<TSharedPtr<FJsonValue>>* Results = Compile.IsValid() ? AgentJson::GetArray(*Compile, TEXT("results")) : nullptr;
	if (Results && Results->Num() == 1)
	{
		TestTrue(TEXT("saved"), AgentJson::GetBool(*(*Results)[0]->AsObject(), TEXT("saved"), false));
	}

	// reload from disk keeps the function
	AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'blueprint.reload','params':{'asset':'%s'}}"), *Asset));
	Summary = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'blueprint.summary','params':{'asset':'%s'}}"), *Asset)));
	TestEqual(TEXT("function survives reload"), AgentTest::Int(Summary, TEXT("functions")), 1);

	// batch atomic rollback: the second op fails, the first must be undone
	TSharedPtr<FJsonObject> BatchResult = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'batch','params':{'atomic':true,'ops':[{'cmd':'blueprint.variable','params':{'asset':'%s','op':'add','name':'Temp','type':'int'}},{'cmd':'blueprint.variable','params':{'asset':'%s','op':'add','name':'Temp','type':'int'}}]}}"), *Asset, *Asset)));
	TestTrue(TEXT("batch aborted"), BatchResult.IsValid() && AgentJson::GetBool(*BatchResult, TEXT("aborted")));
	Summary = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'blueprint.summary','params':{'asset':'%s'}}"), *Asset)));
	TestEqual(TEXT("batch rolled back variable"), AgentTest::Int(Summary, TEXT("variables")), 1);

	ClaudeAgentTest_DeleteAsset(Asset);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
