// v1.1 WS-17 §3 (D64, D65, INV-130): the window's mode entry and actions, decided before the window exists
// (gui/WindowSetup.h), and the elevated window's --intent. Over WS-9's fake host (the Linux legs; the same code is built
// into the Windows window, whose native rows are tst_GuiNative::standardUserWindow and
// guard_installed_before_first_object_gui).

#include "app/Dispatch.h"
#include "cli/CommandLine.h"
#include "gui/WindowSetup.h"
#include "support/FakeCliHost.h"

#include <QTest>

using namespace asb;
using namespace asb::gui;

class tst_GuiMain : public QObject {
    Q_OBJECT

private slots:
    // A standard user's window: the job of its own (never the v1.0 guard, which blocks runas), read-only actions, and
    // the apply host's Platform is never built: no Applier object, ever (INV-130).
    void standardUserBuildsNoApplier()
    {
        test::Rig rig;
        test::FakeCliHost view(rig), apply(rig);
        apply.isElevated = false;
        const WindowRole role = enterWindowGuard(view, &apply);
        QCOMPARE(role, WindowRole::StandardUser);
        QCOMPARE(view.calls, QStringList{QStringLiteral("standardUserJob")});
        QCOMPARE(apply.calls, QStringList{QStringLiteral("elevated")});
        WindowActions made = windowActions(&apply, role, Installation::Flavor::Windows);
        QVERIFY(!made.operations);
        QVERIFY(made.actions);
        QVERIFY(dynamic_cast<InProcessActions*>(made.actions.get()) == nullptr);
        QVERIFY(!made.actions->available());
        QCOMPARE(made.actions->unavailableReason(), standardUserReason());
        QVERIFY2(!apply.calls.contains(QStringLiteral("platform")), qPrintable(apply.calls.join(QLatin1Char(','))));
        // Every lifecycle action is refused with the reason.
        QCOMPARE(made.actions->lifecycleUnavailableReason(), standardUserReason());
    }

    // The elevated window: the elevation is read first, then the v1.0 guard, and only then the Applier's Platform.
    void elevatedGuardBeforeApplier()
    {
        test::Rig rig;
        test::FakeCliHost view(rig), apply(rig);
        apply.isElevated = true;
        const WindowRole role = enterWindowGuard(view, &apply);
        QCOMPARE(role, WindowRole::Elevated);
        QCOMPARE(view.calls, QStringList{QStringLiteral("guard")});
        QCOMPARE(apply.calls, QStringList{QStringLiteral("elevated")});
        WindowActions made = windowActions(&apply, role, Installation::Flavor::Windows);
        QVERIFY(made.operations);
        QVERIFY(dynamic_cast<InProcessActions*>(made.actions.get()) != nullptr);
        QCOMPARE(apply.calls, (QStringList{QStringLiteral("elevated"), QStringLiteral("platform")}));
    }

    // No apply host (a window that could not determine one) is a standard user's window.
    void noApplyHostIsReadOnly()
    {
        test::Rig rig;
        test::FakeCliHost view(rig);
        QCOMPARE(enterWindowGuard(view, nullptr), WindowRole::StandardUser);
        WindowActions made = windowActions(nullptr, WindowRole::Elevated, Installation::Flavor::Windows);
        QVERIFY(!made.operations);
        QVERIFY(!made.actions->available());
    }

    // --intent=<install|update|uninstall|none>, only with --gui; anything else is a usage error (WS-17 §3).
    void intentParsing_data()
    {
        QTest::addColumn<QStringList>("args");
        QTest::addColumn<QString>("intent");   // empty: a usage error
        QTest::newRow("install") << QStringList{"--gui", "--intent=install"} << "install";
        QTest::newRow("update") << QStringList{"--gui", "--intent=update"} << "update";
        QTest::newRow("uninstall-upper") << QStringList{"--intent=UNINSTALL", "--gui"} << "uninstall";
        QTest::newRow("none") << QStringList{"--gui", "--intent=none"} << "none";
        QTest::newRow("other-word") << QStringList{"--gui", "--intent=repair"} << "";
        QTest::newRow("empty-word") << QStringList{"--gui", "--intent="} << "";
        QTest::newRow("twice") << QStringList{"--gui", "--intent=install", "--intent=install"} << "";
        QTest::newRow("without-gui") << QStringList{"--status", "--intent=install"} << "";
        QTest::newRow("alone") << QStringList{"--intent=install"} << "";
        QTest::newRow("settings-from-is-v1.2") << QStringList{"--gui", "--intent=install", "--settings-from=x"} << "";
    }

    void intentParsing()
    {
        QFETCH(QStringList, args);
        QFETCH(QString, intent);
        const cli::Parsed p = cli::parse(args);
        if (intent.isEmpty()) {
            QVERIFY2(!p.ok(), qPrintable(args.join(QLatin1Char(' '))));
            return;
        }
        QVERIFY2(p.ok(), qPrintable(p.usageError));
        QCOMPARE(p.command, cli::Command::Gui);
        QCOMPARE(p.intent.value_or(QString()), intent);
        // The GUI executable's decision carries it to the window (Windows: no display check).
        app::DispatchEnv env;
        env.windows = true;
        std::vector<std::string> a;
        for (const QString& s : args) a.push_back(s.toStdString());
        const app::Decision d = app::decide(app::BuildKind::Gui, env, a);
        QCOMPARE(d.action, app::Action::Gui);
        QCOMPARE(d.parsed.intent.value_or(QString()), intent);
    }
};

QTEST_GUILESS_MAIN(tst_GuiMain)
#include "tst_GuiMain.moc"
