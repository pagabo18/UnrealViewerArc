#include "AgentTestUtils.h"
#include "EditorAssetLibrary.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

void ClaudeAgentTest_DeleteAsset(const FString& PackagePath);

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaudeAgentWidgetRoundtripTest, "ClaudeBlueprintAgent.Widget.Roundtrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FClaudeAgentWidgetRoundtripTest::RunTest(const FString& Parameters)
{
	const FString Asset = FString(AgentTest::TestFolder) + TEXT("/WBP_AgentRoundtrip");
	ClaudeAgentTest_DeleteAsset(Asset);

	TSharedPtr<FJsonObject> Created = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'assets.create_blueprint','params':{'path':'%s','parent':'UserWidget','kind':'widget'}}"), *Asset)));
	TestTrue(TEXT("widget blueprint created"), Created.IsValid() && AgentJson::GetBool(*Created, TEXT("widget")));

	// build: Canvas > VB_Menu > BTN_Play > TXT_Play
	AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'widget.add','params':{'asset':'%s','class':'VerticalBox','name':'VB_Menu','parent':'CanvasPanel'}}"), *Asset));
	AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'widget.add','params':{'asset':'%s','class':'Button','name':'BTN_Play','parent':'VB_Menu','slot':{'Padding':'(Left=4,Top=8,Right=4,Bottom=8)'}}}"), *Asset));
	AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'widget.add','params':{'asset':'%s','class':'TextBlock','name':'TXT_Play','parent':'BTN_Play','properties':{'Text':'PLAY'}}}"), *Asset));
	AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'widget.add','params':{'asset':'%s','class':'Button','name':'BTN_Exit','parent':'VB_Menu'}}"), *Asset));

	// clone BTN_Play -> BTN_Settings between Play and Exit with child text override
	TSharedPtr<FJsonObject> Clone = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'widget.clone','params':{'asset':'%s','source':'BTN_Play','new_name':'BTN_Settings','insert_after':'BTN_Play','children':{'TXT_Play':{'Text':'SETTINGS'}}}}"), *Asset)));
	TestEqual(TEXT("clone name"), AgentTest::Str(Clone, TEXT("widget")), FString(TEXT("BTN_Settings")));
	TestEqual(TEXT("clone index"), AgentTest::Int(Clone, TEXT("index")), 1);

	// tree: names, order, style sharing
	TSharedPtr<FJsonObject> Tree = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'widget.tree','params':{'asset':'%s'}}"), *Asset)));
	const TArray<TSharedPtr<FJsonValue>>* Widgets = Tree.IsValid() ? AgentJson::GetArray(*Tree, TEXT("widgets")) : nullptr;
	TestTrue(TEXT("tree has 7 widgets"), Widgets && Widgets->Num() == 7);
	FString StylePlay, StyleSettings, TextSettingsName;
	if (Widgets)
	{
		for (const TSharedPtr<FJsonValue>& Item : *Widgets)
		{
			const FString Name = AgentJson::GetString(*Item->AsObject(), TEXT("name"));
			if (Name == TEXT("BTN_Play")) { StylePlay = AgentJson::GetString(*Item->AsObject(), TEXT("style")); }
			if (Name == TEXT("BTN_Settings")) { StyleSettings = AgentJson::GetString(*Item->AsObject(), TEXT("style")); }
			if (Name == TEXT("TXT_Settings")) { TextSettingsName = AgentJson::GetString(*Item->AsObject(), TEXT("text")); }
		}
	}
	TestTrue(TEXT("cloned button shares style"), !StylePlay.IsEmpty() && StylePlay == StyleSettings);
	TestEqual(TEXT("child renamed and text overridden"), TextSettingsName, FString(TEXT("SETTINGS")));

	// modify
	TSharedPtr<FJsonObject> Set = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'widget.set','params':{'asset':'%s','widget':'TXT_Settings','properties':{'Text':'OPTIONS'}}}"), *Asset)));
	TSharedPtr<FJsonObject> Inspect = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'widget.inspect','params':{'asset':'%s','widget':'TXT_Settings'}}"), *Asset)));
	TSharedPtr<FJsonObject> Props = Inspect.IsValid() ? AgentJson::GetObject(*Inspect, TEXT("props")) : nullptr;
	TestEqual(TEXT("text updated"), Props.IsValid() ? AgentJson::GetString(*Props, TEXT("Text")) : FString(), FString(TEXT("OPTIONS")));

	// bind OnClicked with a new function
	TSharedPtr<FJsonObject> Bound = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'widget.bind_event','params':{'asset':'%s','widget':'BTN_Settings','event':'OnClicked','function':'OpenSettings','create_function':true}}"), *Asset)));
	TestTrue(TEXT("event node created"), Bound.IsValid() && !AgentTest::Str(Bound, TEXT("node")).IsEmpty());
	TestTrue(TEXT("call node created"), Bound.IsValid() && !AgentTest::Str(Bound, TEXT("call_node")).IsEmpty());

	// compile + preview layout + save
	TSharedPtr<FJsonObject> Compile = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'blueprint.compile','params':{'assets':['%s'],'save':true}}"), *Asset)));
	TestEqual(TEXT("widget compiles"), AgentTest::Int(Compile, TEXT("succeeded")), 1);
	TSharedPtr<FJsonObject> Preview = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'widget.preview','params':{'asset':'%s','width':800,'height':600,'image':true}}"), *Asset)));
	const TArray<TSharedPtr<FJsonValue>>* Layout = Preview.IsValid() ? AgentJson::GetArray(*Preview, TEXT("layout")) : nullptr;
	TestTrue(TEXT("layout produced"), Layout && Layout->Num() >= 7);
	TestTrue(TEXT("image written"), Preview.IsValid() && !AgentTest::Str(Preview, TEXT("image")).IsEmpty());

	// move + remove
	AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'widget.move','params':{'asset':'%s','widget':'BTN_Exit','parent':'VB_Menu','index':0}}"), *Asset));
	AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'widget.remove','params':{'asset':'%s','widget':'BTN_Play'}}"), *Asset));
	Tree = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'widget.tree','params':{'asset':'%s','styles':false}}"), *Asset)));
	TestEqual(TEXT("5 widgets after remove"), AgentTest::Int(Tree, TEXT("total")), 5);

	// reload keeps the saved state (before move/remove) since we did not save after
	AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'blueprint.reload','params':{'asset':'%s'}}"), *Asset));
	Tree = AgentTest::Result(AgentTest::Exec(*this, FString::Printf(TEXT("{'cmd':'widget.tree','params':{'asset':'%s','styles':false}}"), *Asset)));
	TestEqual(TEXT("reload restores 7 widgets"), AgentTest::Int(Tree, TEXT("total")), 7);

	ClaudeAgentTest_DeleteAsset(Asset);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
