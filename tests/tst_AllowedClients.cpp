// INV-14: the allow-list is validated per token, plus the keyword policy seam WS-7 pins.

#include "core/AllowedClients.h"
#include "core/BridgeConfig.h"
#include "core/FirewallKeywords.h"

#include <QTest>

#include <algorithm>

using namespace asb;

namespace {

QStringList errors(const QString& clients)
{
    PortMapping p;
    p.name = QStringLiteral("Amp");
    p.comPort = QStringLiteral("COM3");
    p.tcpPort = 4001;
    p.bindAddress = QStringLiteral("0.0.0.0");
    p.allowedClients = clients;
    BridgeConfig c;
    c.ports = {p};
    return c.validate();
}

}  // namespace

class tst_AllowedClients : public QObject {
    Q_OBJECT

private slots:
    void validTokensPass_data();
    void validTokensPass();
    void invalidTokensFail_data();
    void invalidTokensFail();
    void theMessageNamesTheBadToken();
    void theCharacterFenceStillRejectsQuotes();
    void oneMessagePerBadTokenInOrder();
    void refusedKeywordsHaveTheirOwnMessage_data();
    void refusedKeywordsHaveTheirOwnMessage();
    void keywordPolicyIsASeam();
    void acceptedKeywordsAreASubsetOfTheKnownList();
    // ---- tokens(): the one tokeniser firewall code uses ----------------------------------------
    void tokensAreWhatCheckValidated_data();
    void tokensAreWhatCheckValidated();
    void tokensFailClosed_data();
    void tokensFailClosed();
    void tokensAgreeWithValidate_data();
    void tokensAgreeWithValidate();
};

void tst_AllowedClients::validTokensPass_data()
{
    QTest::addColumn<QString>("clients");
    for (const char* c : {"", "192.168.1.10", "192.168.1.10, 192.168.1.11", "10.0.0.0/8", "0.0.0.0/0", "192.168.1.0/32",
                          "100.64.0.0/10,LocalSubnet", "192.168.1.10-192.168.1.20", "192.168.1.10-192.168.1.10",
                          "fe80::1", "2001:db8::/32", "2001:db8::/128", "::/0", "2001:db8::1-2001:db8::ff", "Any",
                          "localsubnet", "DNS, DHCP, WINS, DefaultGateway", "Internet, Intranet, IntranetRemoteAccess",
                          // octal groups: 010 = 8 < 9
                          "010.0.0.1-9.0.0.0", "1.2.3.4/032", "::ffff:1.2.3.4/96", "2001:db8::-2001:db8::1"})
        QTest::newRow(c) << QString::fromLatin1(c);
}

void tst_AllowedClients::validTokensPass()
{
    QFETCH(QString, clients);
    QCOMPARE(errors(clients), QStringList());
}

// Each row names the rule whose removal it kills. "v4 /33" is the killing row of the INV-14 mutant
// "/33 accepted for v4".
void tst_AllowedClients::invalidTokensFail_data()
{
    QTest::addColumn<QString>("clients");
    QTest::addColumn<QString>("expect");
    const auto row = [](const char* tag, const char* c, const char* e) {
        QTest::newRow(tag) << QString::fromLatin1(c) << QString::fromLatin1(e);
    };
    row("v4 prefix 99", "192.168.1.0/99", "prefix");
    row("v4 /33", "192.168.1.0/33", "prefix");   // 33 is valid only for v6
    row("v6 /129", "2001:db8::/129", "prefix");
    row("missing prefix", "10.0.0.0/", "prefix");
    row("two slashes", "10.0.0.0/8/8", "not an IP");
    row("octet > 255", "300.1.1.1", "not an IP");
    row("shorthand", "1.2.3", "not an IP");   // that IPAddress.TryParse would accept
    row("subnet mask", "10.0.0.0/255.0.0.0", "prefix");
    row("reversed range", "192.168.1.20-192.168.1.10", "first address is after");
    row("mixed family", "192.168.1.1-2001:db8::1", "both ends");
    row("mixed family ordered", "1.1.1.1-2001:db8::1", "both ends");   // would look "ordered" byte-wise
    row("three ends", "1.1.1.1-2.2.2.2-3.3.3.3", "not an IP");
    row("garbage", "banana", "not an IP");
    row("near keyword", "LocalSubnet7", "not an IP");
    row("empty token", "10.0.0.1,,10.0.0.2", "empty");
    row("trailing comma", "10.0.0.1,", "empty");
    row("bad octal", "09.1.1.1", "not an IP");
    row("negative prefix", "1.2.3.4/-1", "not an IP");   // split on '-' first
    row("v6 reversed", "fe80::1-fe80::0", "first address is after");
    row("spaced range", "1.2.3.4 - 1.2.3.5", "not an IP");
    row("provider spelling", "PlayToTargets", "not an IP");
}

void tst_AllowedClients::invalidTokensFail()
{
    QFETCH(QString, clients);
    QFETCH(QString, expect);
    const QStringList e = errors(clients);
    QVERIFY2(std::any_of(e.cbegin(), e.cend(), [&](const QString& m) { return m.contains(expect, Qt::CaseInsensitive); }),
             qPrintable(e.join(QLatin1Char('|'))));
}

void tst_AllowedClients::theMessageNamesTheBadToken()
{
    const QStringList e = errors(QStringLiteral("10.0.0.1, 192.168.1.0/99"));
    QCOMPARE(e.size(), 1);
    QVERIFY(e[0].contains(QStringLiteral("'192.168.1.0/99'")));
    QVERIFY(e[0].contains(QStringLiteral("Amp")));
}

// The character fence still runs first: a quote never reaches the semantic check.
void tst_AllowedClients::theCharacterFenceStillRejectsQuotes()
{
    QCOMPARE(errors(QStringLiteral("1.2.3.4'; Remove-Item")),
             QStringList{QStringLiteral("Amp: allowed clients may only list IP addresses, ranges or LocalSubnet.")});
}

void tst_AllowedClients::oneMessagePerBadTokenInOrder()
{
    QCOMPARE(AllowedClients::check(QStringLiteral("banana, 10.0.0.0/33, ,1.2.3.4")),
             (QStringList{
                 QStringLiteral("allowed client 'banana' is not an IP address, CIDR block (10.0.0.0/8), address range "
                                "(a-b) or a Windows keyword such as LocalSubnet."),
                 QStringLiteral("allowed client '10.0.0.0/33' has an invalid prefix: use /0 to /32 for IPv4 (subnet "
                                "masks are not accepted)."),
                 QStringLiteral("allowed clients has an empty entry (two commas in a row, or a trailing comma)."),
             }));
    QCOMPARE(AllowedClients::check(QStringLiteral("   ")), QStringList());
}

void tst_AllowedClients::refusedKeywordsHaveTheirOwnMessage_data()
{
    QTest::addColumn<QString>("keyword");
    for (const char* k : {"LocalSubnet4", "LocalSubnet6", "PlayToDevice", "playtodevice"})
        QTest::newRow(k) << QString::fromLatin1(k);
}

void tst_AllowedClients::refusedKeywordsHaveTheirOwnMessage()
{
    QFETCH(QString, keyword);
    // The pinned set accepts every known keyword (WS-7 fw_keyword_matrix); a build whose provider refuses one is
    // modelled with the policy seam: a known keyword outside the accepted set gets its own message.
    QVERIFY(AllowedClients::check(keyword).isEmpty());
    const std::array<std::string_view, 2> narrower = {"Any", "LocalSubnet"};
    QCOMPARE(AllowedClients::check(keyword, narrower, kKnownFirewallKeywords),
             QStringList{QStringLiteral("allowed client '%1' is a keyword the Windows firewall does not accept; list "
                                        "the addresses instead.")
                             .arg(keyword)});
}

// WS-7 pins the accepted set from fw_keyword_matrix; widening it is all it takes.
void tst_AllowedClients::keywordPolicyIsASeam()
{
    const std::array<std::string_view, 2> accepted = {"LocalSubnet", "PlayToDevice"};
    QCOMPARE(AllowedClients::check(QStringLiteral("PlayToDevice, localsubnet"), accepted, kKnownFirewallKeywords),
             QStringList());
    QCOMPARE(AllowedClients::check(QStringLiteral("DNS"), accepted, kKnownFirewallKeywords).size(), 1);
}

void tst_AllowedClients::acceptedKeywordsAreASubsetOfTheKnownList()
{
    for (std::string_view k : kAcceptedFirewallKeywords)
        QVERIFY2(std::find(kKnownFirewallKeywords.begin(), kKnownFirewallKeywords.end(), k) !=
                     kKnownFirewallKeywords.end(),
                 k.data());
}

void tst_AllowedClients::tokensAreWhatCheckValidated_data()
{
    QTest::addColumn<QString>("clients");
    QTest::addColumn<QStringList>("expected");
    QTest::newRow("blank") << QString() << QStringList();
    QTest::newRow("spaces") << QStringLiteral("   ") << QStringList();
    QTest::newRow("only a newline") << QStringLiteral("\n") << QStringList();   // blank, any address
    QTest::newRow("one") << QStringLiteral("1.2.3.4") << QStringList{QStringLiteral("1.2.3.4")};
    QTest::newRow("trimmed") << QStringLiteral(" 10.0.0.1 , LocalSubnet ,2001:db8::/32")
                             << QStringList{QStringLiteral("10.0.0.1"), QStringLiteral("LocalSubnet"),
                                            QStringLiteral("2001:db8::/32")};
    // The fence admits one final newline (`$`); the token handed out never carries it.
    QTest::newRow("trailing newline") << QStringLiteral("1.2.3.4\n") << QStringList{QStringLiteral("1.2.3.4")};
    QTest::newRow("range, newline") << QStringLiteral("10.0.0.1-10.0.0.9\n") << QStringList{QStringLiteral("10.0.0.1-10.0.0.9")};
    QTest::newRow("keyword as written") << QStringLiteral("localsubnet") << QStringList{QStringLiteral("localsubnet")};
}

void tst_AllowedClients::tokensAreWhatCheckValidated()
{
    QFETCH(QString, clients);
    QFETCH(QStringList, expected);
    QCOMPARE(AllowedClients::check(clients), QStringList());
    const auto t = AllowedClients::tokens(clients);
    QVERIFY(t);
    QCOMPARE(*t, expected);
}

// Anything validate() refuses gives no tokens at all, so firewall code can never read an invalid value as
// an empty list (any address). Each row is non-blank, so an empty list would be the dangerous answer.
void tst_AllowedClients::tokensFailClosed_data()
{
    QTest::addColumn<QString>("clients");
    for (const QString& c : {QStringLiteral("banana"), QStringLiteral("10.0.0.1,"), QStringLiteral(","),
                             QStringLiteral(" , "), QStringLiteral("10.0.0.0/33"), QStringLiteral("PlayToTargets"),
                             QStringLiteral("1.2.3.4'; Remove-Item"), QStringLiteral("1.2.3.4\n,5.6.7.8"),
                             QStringLiteral("1.2.3.4\r"), QStringLiteral("1.2.3.4\n\n"), QStringLiteral("\uFEFF"),
                             QStringLiteral("\u00A0"), QStringLiteral("\u2028"), QStringLiteral("\u3000"),
                             QStringLiteral("10.0.0.1\u200B"), QStringLiteral("Local\u017Fubnet"),
                             QString(501, QLatin1Char('1')), QString(QChar(0xD800))})
        QTest::newRow(qPrintable(QString::fromLatin1(c.toUtf8().toHex()))) << c;
}

void tst_AllowedClients::tokensFailClosed()
{
    QFETCH(QString, clients);
    QVERIFY(!errors(clients).isEmpty());   // validate() refuses it ...
    QVERIFY(!AllowedClients::tokens(clients));   // ... and tokens() hands out nothing
}

// tokens() succeeds exactly when validate() reports nothing about the allow-list, over every allow-list
// row of the validation tests plus the decoding-audit strings.
void tst_AllowedClients::tokensAgreeWithValidate_data()
{
    QTest::addColumn<QString>("clients");
    for (const char* c : {"", "1.2.3.4", "1.2.3.4\n", "a b", "10.0.0.1 ,10.0.0.2", "1.2.3.4 - 1.2.3.5", "::", ":::",
                          "LocalSubnet\n", "10.0.0.0/8\n", "\n", "\t10.0.0.1", "10.0.0.1\t", "Any", "any, 10.0.0.1"})
        QTest::newRow(c) << QString::fromLatin1(c);
}

void tst_AllowedClients::tokensAgreeWithValidate()
{
    QFETCH(QString, clients);
    QCOMPARE(bool(AllowedClients::tokens(clients)), errors(clients).isEmpty());
}

QTEST_GUILESS_MAIN(tst_AllowedClients)
#include "tst_AllowedClients.moc"
