#include "gui/WindowSetup.h"

namespace asb::gui {

WindowRole enterWindowGuard(cli::Host& view, cli::Host* apply)
{
    // The elevation is read before any guard: the token only, no Platform or Applier object (INV-130).
    const WindowRole role = apply && apply->elevated() ? WindowRole::Elevated : WindowRole::StandardUser;
    if (role == WindowRole::Elevated) view.enterGuardedMode();   // v1.0: ActiveProcessLimit 1, before any Applier object
    else view.enterStandardUserJob();                             // limit 2, kill-on-close; this window builds no Applier
    return role;
}

QString standardUserReason()
{
    return QStringLiteral("Read-only: changing the bridge needs administrator rights.");
}

WindowActions windowActions(cli::Host* apply, WindowRole role, Installation::Flavor flavor)
{
    WindowActions out;
    if (role != WindowRole::Elevated || !apply) {
        out.actions = std::make_unique<ReadOnlyActions>(flavor, standardUserReason());
        return out;
    }
    QString applyError;
    int applyCode = 0;
    if (Platform* ap = apply->platform(&applyError, &applyCode)) {
        out.operations = std::make_unique<OperationHost>(*ap);
        out.actions = std::make_unique<InProcessActions>(*out.operations, flavor);
    } else {
        out.actions = std::make_unique<ReadOnlyActions>(
            flavor, QStringLiteral("Read-only: the bridge cannot be changed from this window (%1).").arg(applyError));
    }
    return out;
}

}  // namespace asb::gui
