# Compact widget format

```
A5 WBP_MainMenu (UserWidget < UserWidget) 10 widgets
CanvasPanel CanvasPanel
├─ IMG_Background Image S1
└─ VB_Menu VerticalBox
   ├─ TXT_Title TextBlock "MY GAME" S2
   ├─ BTN_Play Button S3 var events:OnClicked
   │  └─ TXT_Play TextBlock "PLAY" S4
   ├─ BTN_Settings Button S3 var events:OnClicked
   │  └─ TXT_Settings TextBlock "SETTINGS" S4
   └─ BTN_Exit Button S3 var events:OnClicked
      └─ TXT_Exit TextBlock "EXIT" S4
Styles:
S3 Button (3): WidgetStyle=(Normal=(...)) IsFocusable=True Slot.Padding=(Top=12,Bottom=12) Slot.HorizontalAlignment=HAlign_Fill
S4 TextBlock (3): Font=(FontObject=/Game/UI/Fonts/F_MainMenu,Size=32) ColorAndOpacity=(...) Justification=Center
Animations: FadeIn
```

- `S#` = style fingerprint: hash of non-default, non-content properties (+ slot settings, minus canvas offsets). Same `S#` ⇒ visually identical style ⇒ safe to clone.
- `var` = exposed as variable (needed for event binding; `bind_widget_event` sets it).
- `"text"` shows the Text property; `(Collapsed)` etc. shows non-visible visibility.
- `inspect_widget` prints `props:` (non-default only), `slot Class: ...`, `children`, `events` (bindable delegates) and `bound` (existing bindings with node guids).
- Property paths: `Font.Size`, `WidgetStyle.Normal.TintColor.SpecifiedColor`, `Slot.Padding` (via `slot=`), `Brush.ResourceObject`.
- Values: text as plain string; structs `(X=1,Y=2)`; colors `(R=1,G=1,B=1,A=1)` or `(SpecifiedColor=(R=..))` for SlateColor; enums by name (`HAlign_Fill`, `Visible`); assets by `/Game/...` path; booleans `true/false`.
