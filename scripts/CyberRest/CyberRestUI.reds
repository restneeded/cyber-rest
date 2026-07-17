module CyberRest.UI

import Codeware.UI.*
import CyberRest.Core.*

// =============================================================================================
//  cyber.rest - main-menu multiplayer controls (clickable, no hotkeys, no file editing).
//
//  START SCREEN adds two items:
//    "Host Game"  -> loads your latest save, then auto-hosts once you're in Night City.
//    "Join Game"  -> loads your latest save, then a type-the-address box pops the instant the
//                    world is ready (a text box can't render on the cold title screen, so it has
//                    to appear once you're in-game).
//
//  A ScriptableService carries the chosen role across the menu->game load, and Codeware's
//  Session/Ready callback fires the host/join once the world exists. All UI is built from
//  Codeware primitives at runtime (no custom .inkwidget archive needed).
// =============================================================================================

// ---- cross-transition role carried from the menu into the loaded session --------------------
enum CyberRestPendingRole {
    None = 0,
    Host = 1,
    Join = 2,
}

public class CyberRestMPBoot extends ScriptableService {
    private let m_pending: Int32 = 0; // CyberRestPendingRole as Int32 (0 none / 1 host / 2 join)

    // Captured on the start-menu controller (where GetSystemRequestsHandler() is available). The
    // scenario cb that handles our menu click can't reach that accessor, so it loads through this.
    private let m_handler: wref<inkISystemRequestsHandler>;

    private cb func OnLoad() {
        GameInstance.GetCallbackSystem().RegisterCallback(n"Session/Ready", this, n"OnSessionReady");
    }

    public func SetHandler(handler: wref<inkISystemRequestsHandler>) -> Void {
        this.m_handler = handler;
    }

    public func LoadLastSave() -> Void {
        if IsDefined(this.m_handler) {
            this.m_handler.LoadLastCheckpoint(false);
        } else {
            FTLog("[cyber.rest] no system-requests handler captured; can't auto-load. Use Continue, then Esc -> Multiplayer.");
        }
    }

    public func RequestHost() -> Void {
        this.m_pending = EnumInt(CyberRestPendingRole.Host);
        FTLog("[cyber.rest] main-menu: Host Game selected");
    }

    public func RequestJoin() -> Void {
        this.m_pending = EnumInt(CyberRestPendingRole.Join);
        FTLog("[cyber.rest] main-menu: Join Game selected");
    }

    private cb func OnSessionReady(event: ref<GameSessionEvent>) {
        let role = this.m_pending;
        this.m_pending = EnumInt(CyberRestPendingRole.None);
        if role == EnumInt(CyberRestPendingRole.Host) {
            let mp = GameInstance.GetCyberRestSystem();
            if IsDefined(mp) {
                mp.StartHostSession();
            }
        } else {
            if role == EnumInt(CyberRestPendingRole.Join) {
                let controller = GameInstance.GetInkSystem().GetLayer(n"inkHUDLayer").GetGameController() as inkGameController;
                CyberRestJoinPopup.Show(controller);
            }
        }
    }

    // Opens the host/join box on demand (used by the in-game pause-menu entry as a fallback).
    public func ShowPopupNow() -> Void {
        let controller = GameInstance.GetInkSystem().GetLayer(n"inkHUDLayer").GetGameController() as inkGameController;
        CyberRestJoinPopup.Show(controller);
    }
}

// Fetch the boot service from any context (menu or in-game).
public func CyberRestBoot() -> ref<CyberRestMPBoot> {
    return GameInstance.GetScriptableServiceContainer().GetService(n"CyberRest.UI.CyberRestMPBoot") as CyberRestMPBoot;
}

// ---- the type-the-address box (Join) --------------------------------------------------------
public class CyberRestJoinPopup extends InGamePopup {
    protected let m_header: ref<InGamePopupHeader>;
    protected let m_footer: ref<InGamePopupFooter>;
    protected let m_content: ref<InGamePopupContent>;
    protected let m_input: ref<HubTextInput>;

    public static func Show(requester: wref<inkGameController>) -> Void {
        if !IsDefined(requester) {
            return;
        }
        let self: ref<CyberRestJoinPopup> = new CyberRestJoinPopup();
        self.CreateInstance();
        self.Open(requester);
    }

    protected cb func OnCreate() {
        super.OnCreate();
        this.m_container.SetSize(new Vector2(860.0, 460.0));

        this.m_header = InGamePopupHeader.Create();
        this.m_header.SetTitle("CYBER.REST  //  MULTIPLAYER");
        this.m_header.SetFluffRight("free-roam");
        this.m_header.Reparent(this);

        this.m_footer = InGamePopupFooter.Create();
        this.m_footer.SetFluffText("Type the host's address, then HOST or JOIN. Port 7777.");
        this.m_footer.Reparent(this);

        this.m_content = InGamePopupContent.Create();
        this.m_content.Reparent(this);

        let col = new inkVerticalPanel();
        col.SetName(n"col");
        col.SetAnchor(inkEAnchor.Centered);
        col.SetAnchorPoint(new Vector2(0.5, 0.5));
        col.SetChildMargin(new inkMargin(0.0, 16.0, 0.0, 16.0));
        col.Reparent(this.m_content.GetRootCompoundWidget());

        let label = new inkText();
        label.SetName(n"label");
        label.SetFontFamily("base\\gameplay\\gui\\fonts\\raj\\raj.inkfontfamily");
        label.SetFontStyle(n"Medium");
        label.SetFontSize(34);
        label.SetHorizontalAlignment(textHorizontalAlignment.Center);
        label.SetStyle(r"base\\gameplay\\gui\\common\\main_colors.inkstyle");
        label.BindProperty(n"tintColor", n"MainColors.Blue");
        label.SetText("Host address  (LAN IP, or public IP for internet)");
        label.Reparent(col);

        this.m_input = HubTextInput.Create();
        this.m_input.SetName(n"addr");
        this.m_input.Reparent(col);

        let row = new inkHorizontalPanel();
        row.SetName(n"row");
        row.SetHAlign(inkEHorizontalAlign.Center);
        row.SetChildMargin(new inkMargin(20.0, 0.0, 20.0, 0.0));
        row.Reparent(col);

        let hostBtn = SimpleButton.Create();
        hostBtn.SetName(n"host");
        hostBtn.SetText("HOST");
        hostBtn.Reparent(row);
        hostBtn.RegisterToCallback(n"OnBtnClick", this, n"OnHostClick");

        let joinBtn = SimpleButton.Create();
        joinBtn.SetName(n"join");
        joinBtn.SetText("JOIN");
        joinBtn.Reparent(row);
        joinBtn.RegisterToCallback(n"OnBtnClick", this, n"OnJoinClick");
    }

    protected cb func OnHostClick(widget: wref<inkWidget>) -> Bool {
        let mp = GameInstance.GetCyberRestSystem();
        if IsDefined(mp) {
            mp.StartHostSession();
        }
        this.Close();
        return true;
    }

    protected cb func OnJoinClick(widget: wref<inkWidget>) -> Bool {
        let addr = this.m_input.GetText();
        let mp = GameInstance.GetCyberRestSystem();
        if IsDefined(mp) && StrLen(addr) > 0 {
            mp.StartJoinSession(addr);
        } else {
            FTLog("[cyber.rest] JOIN ignored - no address typed (press HOST to host instead)");
        }
        this.Close();
        return true;
    }
}

// ---- START SCREEN menu items ----------------------------------------------------------------
@wrapMethod(SingleplayerMenuGameController)
private func PopulateMenuItemList() -> Void {
    wrappedMethod();
    // Capture the system-requests handler here: `this` is the controller, which HAS the accessor
    // (the scenario cb below does not). Stash it so the click can load the latest save.
    let boot = CyberRestBoot();
    if IsDefined(boot) {
        boot.SetHandler(this.GetSystemRequestsHandler());
    }
    this.AddMenuItem("Host Game", n"OnCyberRestHost");
    this.AddMenuItem("Join Game", n"OnCyberRestJoin");
}

@addMethod(MenuScenario_SingleplayerMenu)
protected cb func OnCyberRestHost() -> Bool {
    let boot = CyberRestBoot();
    if IsDefined(boot) {
        boot.RequestHost();
        boot.LoadLastSave();
    }
    return true;
}

@addMethod(MenuScenario_SingleplayerMenu)
protected cb func OnCyberRestJoin() -> Bool {
    let boot = CyberRestBoot();
    if IsDefined(boot) {
        boot.RequestJoin();
        boot.LoadLastSave();
    }
    return true;
}

// ---- PAUSE MENU fallback (open the box directly while already in-game) -----------------------
@wrapMethod(PauseMenuGameController)
private func PopulateMenuItemList() -> Void {
    wrappedMethod();
    this.AddMenuItem("Multiplayer", n"OnCyberRestPauseMP");
}

@addMethod(MenuScenario_PauseMenu)
protected cb func OnCyberRestPauseMP() -> Bool {
    let boot = CyberRestBoot();
    if IsDefined(boot) {
        boot.ShowPopupNow();
    }
    return true;
}
