// The helper's wire format (WS-9 "Outputs"; master §5.7) and the polkit policy it is reached through (WS-9b): what
// the client encodes the helper parses back to the same request, the configBytes byte for byte; responses parse
// back to the Result; the embedded policy is the packaging file, byte for byte, and says what §5.7 requires.

#include "cli/HelperProtocol.h"
#include "cli/PolkitPolicy.h"
#include "core/ConfigJson.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QXmlStreamReader>

using namespace asb;
using namespace asb::cli;
using namespace asb::cli::helper;

class tst_HelperProtocol : public QObject {
    Q_OBJECT

    // The current data row's request is refused with `why` in the error.
    static void refused()
    {
        QFETCH(QByteArray, input);
        QFETCH(QString, why);
        QString error;
        QVERIFY2(!parseRequest(input, &error), input.constData());
        QVERIFY2(error.contains(why), qPrintable(error));
    }

private slots:
    void requestRoundTrip_data()
    {
        QTest::addColumn<int>("op");
        QTest::addColumn<bool>("withSave");
        QTest::addColumn<int>("kind");
        QTest::addColumn<bool>("supersede");
        QTest::newRow("save-and-apply, new") << int(Op::SaveAndApply) << true << int(Source::New) << false;
        QTest::newRow("save-and-apply, pending") << int(Op::SaveAndApply) << true << int(Source::Pending) << true;
        QTest::newRow("deactivate, committed") << int(Op::Deactivate) << true << int(Source::Committed) << false;
        QTest::newRow("deactivate, no edits") << int(Op::Deactivate) << false << 0 << false;
        QTest::newRow("start") << int(Op::Start) << false << 0 << false;
        QTest::newRow("stop") << int(Op::Stop) << false << 0 << false;
        QTest::newRow("repair") << int(Op::Repair) << false << 0 << false;
        QTest::newRow("discard-pending") << int(Op::DiscardPending) << false << 0 << false;
        QTest::newRow("inspect") << int(Op::Inspect) << false << 0 << false;
    }
    void requestRoundTrip()
    {
        QFETCH(int, op);
        QFETCH(bool, withSave);
        QFETCH(int, kind);
        QFETCH(bool, supersede);
        Request r;
        r.op = Op(op);
        // Bytes JSON would never carry verbatim: a BOM, CRLF, a NUL, invalid UTF-8. They travel exactly.
        QByteArray bytes("\xEF\xBB\xBF{ \"version\" : 1,\r\n\"ports\":[] }");
        bytes.append('\0').append('\xFF');
        if (withSave)
            r.save = SaveRequest{bytes, Source{Source::Kind(kind), kind == Source::New ? QByteArray() : sha256Hex("x").toLatin1()}, supersede};
        QString why;
        const std::optional<Request> back = parseRequest(encodeRequest(r), &why);
        QVERIFY2(back, qPrintable(why));
        QCOMPARE(back->op, r.op);
        QCOMPARE(back->save.has_value(), withSave);
        if (withSave) {
            QCOMPARE(back->save->config, bytes);
            QCOMPARE(int(back->save->source.kind), kind);
            QCOMPARE(back->save->source.sha256, r.save->source.sha256);
            QCOMPARE(back->save->supersedePending, supersede);
        }
    }
    void wireNamesAreTheSpecs()
    {
        QCOMPARE(QJsonDocument::fromJson(encodeRequest(Request{Op::DiscardPending, std::nullopt})).object(),
                 (QJsonObject{{"op", "discard-pending"}}));
        Request r{Op::SaveAndApply, SaveRequest{QByteArray("{}"), Source{Source::Pending, sha256Hex("p").toLatin1()}, true}};
        const QJsonObject o = QJsonDocument::fromJson(encodeRequest(r)).object();
        QCOMPARE(o.value("op").toString(), QStringLiteral("save-and-apply"));
        QCOMPARE(o.value("configBytes").toString(), QStringLiteral("e30="));
        QCOMPARE(o.value("source").toObject().value("kind").toString(), QStringLiteral("pending"));
        QCOMPARE(o.value("source").toObject().value("sha256").toString(), sha256Hex("p"));
        QCOMPARE(o.value("supersedePending").toBool(), true);
    }
    // Whitespace around the one value is allowed (the spec: "trailing whitespace only").
    void surroundingWhitespaceIsAllowed()
    {
        QString why;
        QVERIFY2(parseRequest(" \t\r\n{\"op\":\"stop\"} \t\r\n\n", &why), qPrintable(why));
    }
    void resultResponseRoundTrip()
    {
        Result r;
        r.outcome = Outcome::Uncommitted;
        r.persisted = Persisted::Staged;
        r.persistedSha256 = sha256Hex("n").toLatin1();
        r.step = QStringLiteral("firewall");
        r.message = QStringLiteral("Could not update the firewall: ünïcode.");
        r.postcondition = QStringLiteral("could not prove the bridge stopped");
        r.fileChanges = {QStringLiteral("a"), QStringLiteral("b")};
        bool ok = true;
        QString why;
        const std::optional<Result> back = parseResultResponse(encodeResult(r), &ok, &why);
        QVERIFY2(back, qPrintable(why));
        QCOMPARE(ok, false);
        QCOMPARE(back->outcome, r.outcome);
        QCOMPARE(back->persisted, r.persisted);
        QCOMPARE(back->persistedSha256, r.persistedSha256);
        QCOMPARE(back->step, r.step);
        QCOMPARE(back->message, r.message);
        QCOMPARE(back->postcondition, r.postcondition);
        QCOMPARE(back->fileChanges, r.fileChanges);
    }
    // persistedSha256 is present only when something was persisted; optional members are omitted when empty.
    void resultResponseShape()
    {
        Result r;
        r.message = QStringLiteral("nothing");
        const QJsonObject o = QJsonDocument::fromJson(encodeResult(r)).object();
        QCOMPARE(o.keys(), (QStringList{"fileChanges", "message", "ok", "outcome", "persisted"}));
        r.outcome = Outcome::Ok;
        r.persisted = Persisted::Committed;
        r.persistedSha256 = sha256Hex("c").toLatin1();
        const QJsonObject c = QJsonDocument::fromJson(encodeResult(r)).object();
        QCOMPARE(c.value("ok").toBool(), true);
        QCOMPARE(c.value("persistedSha256").toString(), sha256Hex("c"));
    }
    void badResponsesAreRefused_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("not json") << QByteArray("Saved and applied");
        QTest::newRow("no ok") << QByteArray(R"({"outcome":"Ok","persisted":"Committed","message":"","fileChanges":[]})");
        QTest::newRow("unknown outcome") << QByteArray(R"({"ok":true,"outcome":"Fine","persisted":"Committed","message":"","fileChanges":[]})");
        QTest::newRow("bad sha") << QByteArray(R"({"ok":true,"outcome":"Ok","persisted":"Committed","persistedSha256":"abc","message":"","fileChanges":[]})");
        QTest::newRow("two values") << QByteArray(R"({"ok":true,"outcome":"Ok","persisted":"None","message":"","fileChanges":[]}{})");
        QTest::newRow("fileChanges not strings") << QByteArray(R"({"ok":true,"outcome":"Ok","persisted":"None","message":"","fileChanges":[1]})");
    }
    void badResponsesAreRefused()
    {
        QFETCH(QByteArray, bytes);
        bool ok = false;
        QString why;
        QVERIFY(!parseResultResponse(bytes, &ok, &why));
        QVERIFY(!why.isEmpty());
    }
    void inspectResponseRoundTrip()
    {
        const InspectReport i{QStringLiteral("Recreate"), QStringLiteral("owned by uid 1000"), QStringLiteral("Current"),
                              QStringLiteral("Running")};
        const QJsonObject o = QJsonDocument::fromJson(encodeInspect(i, true)).object();
        QCOMPARE(o.keys(), (QStringList{"installation", "ok", "reason", "service", "trust"}));
        bool ok = false;
        QString why;
        const std::optional<InspectReport> back = parseInspectResponse(encodeInspect(i, true), &ok, &why);
        QVERIFY2(back, qPrintable(why));
        QVERIFY(ok);
        QCOMPARE(back->trust, i.trust);
        QCOMPARE(back->reason, i.reason);
        QCOMPARE(back->installation, i.installation);
        QCOMPARE(back->service, i.service);
    }

    // ---- the polkit policy (WS-9b): one source, embedded byte for byte ---------------------------------

    // ---- v1.1 (WS-15, INV-123): three new ops; the v1.0 forms are frozen byte for byte ----------------------------
    void installOpRoundTrip()
    {
        Request bare;
        bare.op = Op::Install;
        QCOMPARE(encodeRequest(bare), QByteArray("{\"op\":\"install\"}\n"));
        QString why;
        std::optional<Request> back = parseRequest(encodeRequest(bare), &why);
        QVERIFY2(back, qPrintable(why));
        QCOMPARE(int(back->op), int(Op::Install));
        QVERIFY(!back->save);

        Request withConfig;
        withConfig.op = Op::Install;
        withConfig.save = SaveRequest{QByteArray("{ \"version\": 1 }\n"), Source{Source::Committed, QByteArray(64, 'a')}, true};
        QCOMPARE(encodeRequest(withConfig),
                 QByteArray("{\"configBytes\":\"eyAidmVyc2lvbiI6IDEgfQo=\",\"op\":\"install\",\"source\":{\"kind\":\"committed\","
                            "\"sha256\":\"") + QByteArray(64, 'a') + "\"},\"supersedePending\":true}\n");
        back = parseRequest(encodeRequest(withConfig), &why);
        QVERIFY2(back && back->save, qPrintable(why));
        QCOMPARE(back->save->config, withConfig.save->config);
        QCOMPARE(int(back->save->source.kind), int(Source::Committed));
        QVERIFY(back->save->supersedePending);
    }
    void installOpStrict_data()
    {
        QTest::addColumn<QByteArray>("input");
        QTest::addColumn<QString>("why");
        QTest::newRow("source alone") << QByteArray(R"({"op":"install","source":{"kind":"new"}})") << "source and supersedePending go with configBytes";
        QTest::newRow("supersede alone") << QByteArray(R"({"op":"install","supersedePending":true})") << "source and supersedePending go with configBytes";
        QTest::newRow("removeSettings") << QByteArray(R"({"op":"install","removeSettings":false})") << "unknown member \"removeSettings\"";
        QTest::newRow("bytes without source") << QByteArray(R"({"op":"install","configBytes":"e30="})") << "source must be an object";
        QTest::newRow("inexact base64") << QByteArray(R"({"op":"install","configBytes":"e30","source":{"kind":"new"}})") << "configBytes is not exact base64";
    }
    void installOpStrict() { refused(); }

    void uninstallOpRoundTrip()
    {
        for (const bool remove : {false, true}) {
            Request r;
            r.op = Op::Uninstall;
            r.removeSettings = remove;
            QCOMPARE(encodeRequest(r), remove ? QByteArray("{\"op\":\"uninstall\",\"removeSettings\":true}\n")
                                              : QByteArray("{\"op\":\"uninstall\",\"removeSettings\":false}\n"));
            QString why;
            const std::optional<Request> back = parseRequest(encodeRequest(r), &why);
            QVERIFY2(back, qPrintable(why));
            QCOMPARE(int(back->op), int(Op::Uninstall));
            QCOMPARE(back->removeSettings, remove);
            QVERIFY(!back->save);
        }
    }
    void uninstallOpStrict_data()
    {
        QTest::addColumn<QByteArray>("input");
        QTest::addColumn<QString>("why");
        QTest::newRow("no removeSettings") << QByteArray(R"({"op":"uninstall"})") << "removeSettings must be true or false";
        QTest::newRow("a string") << QByteArray(R"({"op":"uninstall","removeSettings":"true"})") << "removeSettings must be true or false";
        QTest::newRow("a number") << QByteArray(R"({"op":"uninstall","removeSettings":1})") << "removeSettings must be true or false";
        QTest::newRow("a config") << QByteArray(R"({"op":"uninstall","removeSettings":false,"configBytes":"e30="})") << "unknown member \"configBytes\"";
        QTest::newRow("twice") << QByteArray(R"({"op":"uninstall","removeSettings":false,"removeSettings":true})") << "\"removeSettings\" is given twice";
    }
    void uninstallOpStrict() { refused(); }

    void discardPendingIfOpRoundTrip()
    {
        Request r;
        r.op = Op::DiscardPendingIf;
        r.sha256 = QString(64, QLatin1Char('0'));
        QCOMPARE(encodeRequest(r), QByteArray("{\"op\":\"discard-pending-if\",\"sha256\":\"") + QByteArray(64, '0') + "\"}\n");
        QString why;
        const std::optional<Request> back = parseRequest(encodeRequest(r), &why);
        QVERIFY2(back, qPrintable(why));
        QCOMPARE(int(back->op), int(Op::DiscardPendingIf));
        QCOMPARE(back->sha256, r.sha256);
    }
    void discardPendingIfOpStrict_data()
    {
        QTest::addColumn<QByteArray>("input");
        QTest::addColumn<QString>("why");
        const QByteArray ok(64, 'b');
        QTest::newRow("no sha256") << QByteArray(R"({"op":"discard-pending-if"})") << "sha256 must be 64 lower-case hex digits";
        QTest::newRow("upper case") << ("{\"op\":\"discard-pending-if\",\"sha256\":\"" + QByteArray(64, 'B') + "\"}") << "sha256 must be 64 lower-case hex digits";
        QTest::newRow("63 digits") << ("{\"op\":\"discard-pending-if\",\"sha256\":\"" + QByteArray(63, 'b') + "\"}") << "sha256 must be 64 lower-case hex digits";
        QTest::newRow("a number") << QByteArray(R"({"op":"discard-pending-if","sha256":12})") << "sha256 must be 64 lower-case hex digits";
        QTest::newRow("extra member") << ("{\"op\":\"discard-pending-if\",\"sha256\":\"" + ok + "\",\"force\":true}") << "unknown member \"force\"";
    }
    void discardPendingIfOpStrict() { refused(); }

    // INV-123: every v1.0 request form and both response forms, byte for byte as v1.0 wrote them; a v1.0 helper refuses
    // anything else, so a member added to a v1.0 op would break every installed v1.0 program. The parse side: the
    // v1.0 forms are accepted, and no v1.1 member is accepted on a v1.0 op.
    void v10OpsByteFrozen()
    {
        const QByteArray cfg("{}");
        const QByteArray sha(64, 'c');
        Request save;
        save.op = Op::SaveAndApply;
        save.save = SaveRequest{cfg, Source{Source::Pending, sha}, true};
        const QByteArray goldenSave = "{\"configBytes\":\"e30=\",\"op\":\"save-and-apply\",\"source\":{\"kind\":\"pending\",\"sha256\":\"" +
                                      sha + "\"},\"supersedePending\":true}\n";
        QCOMPARE(encodeRequest(save), goldenSave);
        save.save = SaveRequest{cfg, Source{Source::New, {}}, false};
        const QByteArray goldenSaveNew = "{\"configBytes\":\"e30=\",\"op\":\"save-and-apply\",\"source\":{\"kind\":\"new\"},\"supersedePending\":false}\n";
        QCOMPARE(encodeRequest(save), goldenSaveNew);
        Request deact;
        deact.op = Op::Deactivate;
        QCOMPARE(encodeRequest(deact), QByteArray("{\"op\":\"deactivate\"}\n"));
        deact.save = SaveRequest{cfg, Source{Source::Committed, sha}, false};
        const QByteArray goldenDeact = "{\"configBytes\":\"e30=\",\"op\":\"deactivate\",\"source\":{\"kind\":\"committed\",\"sha256\":\"" +
                                       sha + "\"},\"supersedePending\":false}\n";
        QCOMPARE(encodeRequest(deact), goldenDeact);
        const std::pair<Op, const char*> plain[] = {{Op::Start, "start"}, {Op::Stop, "stop"}, {Op::Repair, "repair"},
                                                    {Op::DiscardPending, "discard-pending"}, {Op::Inspect, "inspect"}};
        QList<QByteArray> goldens{goldenSave, goldenSaveNew, "{\"op\":\"deactivate\"}\n", goldenDeact};
        for (const auto& [op, name] : plain) {
            const QByteArray golden = QByteArray("{\"op\":\"") + name + "\"}\n";
            QCOMPARE(encodeRequest(Request{op, std::nullopt}), golden);
            goldens << golden;
        }
        for (const QByteArray& g : goldens) {
            QString why;
            QVERIFY2(parseRequest(g, &why), qPrintable(QString::fromLatin1(g) + why));
        }
        // No v1.1 member on a v1.0 op.
        for (const QByteArray& bad : {QByteArray("{\"op\":\"save-and-apply\",\"configBytes\":\"e30=\",\"source\":{\"kind\":\"new\"},\"removeSettings\":false}"),
                                      QByteArray("{\"op\":\"discard-pending\",\"sha256\":\"") + sha + "\"}",
                                      QByteArray("{\"op\":\"stop\",\"removeSettings\":true}"),
                                      QByteArray("{\"op\":\"deactivate\",\"keepInstalledBuild\":true}")}) {
            QString why;
            QVERIFY2(!parseRequest(bad, &why), bad.constData());
        }
        // The responses.
        Result r;
        r.outcome = Outcome::Ok;
        r.persisted = Persisted::Committed;
        r.persistedSha256 = sha;
        r.step = QStringLiteral("commit");
        r.message = QStringLiteral("Done.");
        r.postcondition = QStringLiteral("running");
        r.fileChanges = QStringList{QStringLiteral("wrote x")};
        QCOMPARE(encodeResult(r), "{\"fileChanges\":[\"wrote x\"],\"message\":\"Done.\",\"ok\":true,\"outcome\":\"Ok\",\"persisted\":"
                                  "\"Committed\",\"persistedSha256\":\"" + sha + "\",\"postcondition\":\"running\",\"step\":\"commit\"}\n");
        QCOMPARE(encodeInspect(InspectReport{"Trust", "", "Current", "Running"}, true),
                 QByteArray("{\"installation\":\"Current\",\"ok\":true,\"reason\":\"\",\"service\":\"Running\",\"trust\":\"Trust\"}\n"));
    }

    void policyIsThePackagingFileByteForByte()
    {
        QFile f(QStringLiteral(ASB_SOURCE_DIR "/packaging/linux/org.aethersdr.aetherserialbridge.policy"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QByteArray(kPolkitPolicy), f.readAll());
    }
    // The identifiers moved from agency.cloaked to org.aethersdr (2026-10-22): the action is the helper's, and the
    // packaging file is named after the action's prefix, as polkit's convention has it.
    void policyIdentifiersAreAetherSdrs()
    {
        const QString id = QString::fromLatin1(kPolkitActionId);
        QCOMPARE(id, QStringLiteral("org.aethersdr.aetherserialbridge.helper"));
        const QString prefix = id.left(id.lastIndexOf(QLatin1Char('.')));
        QVERIFY(QFileInfo::exists(QStringLiteral(ASB_SOURCE_DIR "/packaging/linux/") + prefix + QStringLiteral(".policy")));
        QVERIFY(!QByteArray(kPolkitPolicy).contains("agency.cloaked"));
    }
    // §5.7: auth_admin for every subject, exec.path the installed program, the action the helper's id.
    void policySaysWhatTheSpecRequires()
    {
        QXmlStreamReader x{QByteArray(kPolkitPolicy)};
        QStringList actions, defaults;
        QMap<QString, QString> annotations;
        while (!x.atEnd()) {
            x.readNext();
            if (!x.isStartElement()) continue;
            if (x.name() == QLatin1String("action")) actions << x.attributes().value("id").toString();
            else if (x.name() == QLatin1String("allow_any") || x.name() == QLatin1String("allow_inactive") ||
                     x.name() == QLatin1String("allow_active"))
                defaults << x.name().toString() + QLatin1Char('=') + x.readElementText();
            else if (x.name() == QLatin1String("annotate")) {
                const QString key = x.attributes().value("key").toString();
                annotations[key] = x.readElementText();
            }
        }
        QVERIFY2(!x.hasError(), qPrintable(x.errorString()));
        QCOMPARE(actions, (QStringList{QString::fromLatin1(kPolkitActionId)}));
        QCOMPARE(defaults, (QStringList{"allow_any=auth_admin", "allow_inactive=auth_admin", "allow_active=auth_admin"}));
        QCOMPARE(annotations.value("org.freedesktop.policykit.exec.path"), QStringLiteral("/usr/local/bin/aether-serial-bridge"));
        QCOMPARE(annotations.value("org.freedesktop.policykit.exec.argv1"), QStringLiteral("--helper"));
        QCOMPARE(annotations.size(), 2);
    }
};

QTEST_GUILESS_MAIN(tst_HelperProtocol)
#include "tst_HelperProtocol.moc"
