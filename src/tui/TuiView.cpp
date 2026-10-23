#include "tui/TuiView.h"

#include "cli/ConfigEditor.h"
#ifndef _WIN32
#include "cli/StagedHelper.h"
#endif
#include "tui/Logo.h"
#include "tui/TuiHelp.h"
#include "tui/TuiText.h"

#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <QDir>

#include <algorithm>
#include <utility>

namespace asb::tui {

bool isToggle(Field f)
{
    return f == Field::Enabled || f == Field::Dtr || f == Field::Rts;
}

QString autoStartLabel(bool windows)
{
    return windows ? QStringLiteral("Start automatically with Windows") : QStringLiteral("Start automatically at boot");
}

QString elevatedCommand(const TuiState& s)
{
    if (s.windows) {
        // Backslashes whatever this was built on (QDir::toNativeSeparators converts only on Windows itself).
        QString p = s.program.isEmpty() ? QStringLiteral("aether-serial-bridge.exe") : s.program;
        p.replace(QLatin1Char('/'), QLatin1Char('\\'));
        return QStringLiteral("\"%1\" --tui").arg(p);   // a Windows path holds no '"'
    }
    QString p = s.program.isEmpty() ? QStringLiteral("aether-serial-bridge") : s.program;
#ifndef _WIN32
    if (s.readOnlyExecutable) {
        // The GUI executable's elevated TUI is the window again (AppRun runs it for --tui): never name this program.
        if (s.appImage) return cli::staged::terminalCommand(s.program, cli::helper::Op::Install);
        if (s.headlessProgram.isEmpty()) return {};
        p = s.headlessProgram;
    }
#endif
    p.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("sudo '%1' --tui").arg(p);
}

QString appImageOf(const QString& appImage, const QString& appDir, const QString& ownPath)
{
    if (appImage.isEmpty() || appDir.isEmpty() || ownPath.isEmpty()) return {};
    const QString dir = QDir::cleanPath(appDir);
    if (dir == QLatin1String("/") || !QDir::cleanPath(ownPath).startsWith(dir + QLatin1Char('/'))) return {};
    return appImage;
}

QString readOnlyGuiExecutableBanner(const TuiState& s)
{
    const QString command = elevatedCommand(s);
    if (command.isEmpty()) return QString::fromLatin1(text::kReadOnlyGuiExecutableAlone);
    return QString::fromLatin1(s.appImage ? text::kReadOnlyAppImage : text::kReadOnlyGuiExecutable).arg(command);
}

namespace {

using namespace ftxui;

std::string s8(const QString& s)
{
    return s.toStdString();
}

int cells(const std::string& s)
{
    return string_width(s);
}

// `s` cut to at most `width` cells (the last cell an ellipsis when something was cut), then padded with spaces.
std::string fit(const std::string& s, int width, bool utf8)
{
    if (width <= 0) return {};
    if (cells(s) <= width) return s + std::string(std::size_t(width - cells(s)), ' ');
    const std::string more = utf8 ? "…" : "~";
    std::string out;
    int used = 0;
    for (const std::string& g : Utf8ToGlyphs(s)) {
        if (g.empty()) continue;   // the filler after a full-width glyph
        const int w = cells(g);
        if (used + w > width - 1) break;
        out += g;
        used += w;
    }
    out += more;
    used += 1;
    return out + std::string(std::size_t(std::max(0, width - used)), ' ');
}

std::string repeat(const std::string& g, int n)
{
    std::string out;
    for (int i = 0; i < n; ++i) out += g;
    return out;
}

// The last `width` cells of `s` (an ellipsis first when something was cut), padded to `width`.
std::string tailFit(const std::string& s, int width, bool utf8)
{
    if (width <= 0) return {};
    if (cells(s) <= width) return s + std::string(std::size_t(width - cells(s)), ' ');
    const std::string more = utf8 ? "\u2026" : "~";
    std::vector<std::string> glyphs;
    for (const std::string& g : Utf8ToGlyphs(s))
        if (!g.empty()) glyphs.push_back(g);
    std::string out;
    int used = 1;   // the ellipsis
    std::size_t k = glyphs.size();
    while (k > 0 && used + cells(glyphs[k - 1]) <= width) {
        used += cells(glyphs[k - 1]);
        --k;
    }
    out = more;
    for (std::size_t m = k; m < glyphs.size(); ++m) out += glyphs[m];
    return out + std::string(std::size_t(std::max(0, width - used)), ' ');
}

// ---- styled lines -------------------------------------------------------------------------------------
struct Span {
    std::string text;
    std::optional<Role> fg;
    bool bold = false;
    bool inverted = false;
};
using Line = std::vector<Span>;

Role toneRole(Tone t)
{
    switch (t) {
    case Tone::Good: return Role::Teal;
    case Tone::Warn: return Role::Warning;
    case Tone::Bad: return Role::Danger;
    case Tone::Plain: break;
    }
    return Role::Ink;
}

class Painter {
public:
    Painter(const TuiState& s) : m_s(s) {}

    bool utf8() const { return m_s.utf8; }
    bool plainGlyphs() const { return m_s.color == ColorMode::None || !m_s.utf8; }
    std::string ellipsis() const { return m_s.utf8 ? "…" : "..."; }
    std::string rule(int w) const { return repeat(m_s.utf8 ? "─" : "-", w); }
    std::string dotFilled() const { return plainGlyphs() ? "*" : "●"; }
    std::string dotHollow() const { return plainGlyphs() ? "o" : "○"; }
    std::string dotError() const { return plainGlyphs() ? "!" : "●"; }
    std::string check(bool on) const { return on ? "[x]" : "[ ]"; }
    std::string sep() const { return m_s.utf8 ? " \u00b7 " : " - "; }

    Element line(const Line& l, int width) const
    {
        Elements parts;
        int used = 0;
        for (const Span& sp : l) {
            if (used >= width) break;
            std::string t = sp.text;
            if (used + cells(t) > width) t = fit(t, width - used, m_s.utf8);
            used += cells(t);
            parts.push_back(style(ftxui::text(t), sp));
        }
        if (used < width) parts.push_back(ftxui::text(std::string(std::size_t(width - used), ' ')));
        return hbox(std::move(parts));
    }

    Element style(Element e, const Span& sp) const
    {
        if (sp.fg)
            if (const std::optional<Color> c = colorFor(*sp.fg, m_s.color)) e = e | color(*c);
        if (sp.bold) e = e | bold;
        if (sp.inverted) e = e | inverted;
        return e;
    }

private:
    const TuiState& m_s;
};

Line plain(const std::string& t, std::optional<Role> fg = std::nullopt, bool bold = false)
{
    return Line{Span{t, fg, bold, false}};
}

// Wrapped paragraph lines in one tone.
void para(std::vector<Line>& out, const QString& text, int width, std::optional<Role> fg, bool bold = false)
{
    for (const std::string& l : wrap(s8(text), width)) out.push_back(plain(l, fg, bold));
}

// ---- the parts of the ports screen ------------------------------------------------------------------------------
const PortStatus* statusFor(const TuiState& s, const PortMapping& p)
{
    if (!s.status) return nullptr;
    for (const PortStatus& ps : s.status->ports)
        if (ps.tcpPort == p.tcpPort && ps.name == p.displayName()) return &ps;
    for (const PortStatus& ps : s.status->ports)
        if (ps.tcpPort == p.tcpPort) return &ps;
    return nullptr;
}

// The state cell of a port: a dot and words (the state is never colour alone).
Line stateCell(const Painter& pt, const TuiState& s, const PortMapping& p)
{
    if (!p.enabled) return Line{{pt.dotHollow() + " off", Role::Muted}};
    if (!s.status) return Line{{pt.dotHollow() + " not reported", Role::Muted}};
    const PortStatus* ps = statusFor(s, p);
    if (!ps) return Line{{pt.dotHollow() + " not running", Role::Muted}};
    switch (ps->state) {
    case BridgeState::Connected:
        return Line{{pt.dotFilled(), Role::Teal}, {" Connected" + (ps->client ? " · " + s8(*ps->client) : std::string()), Role::Ink}};
    case BridgeState::Listening:
        return Line{{pt.dotFilled(), Role::Cyan},
                    {" Listening" + (ps->lastError ? " · last: " + s8(*ps->lastError) : std::string()), Role::Ink}};
    case BridgeState::Error:
        return Line{{pt.dotError(), Role::Danger}, {" Error" + (ps->lastError ? ": " + s8(*ps->lastError) : std::string()), Role::Danger}};
    case BridgeState::Stopped: break;
    }
    return Line{{pt.dotHollow() + " Stopped", Role::Muted}};
}

// Column widths for a table width.
struct Columns {
    bool device = true, listen = true, port = false;
    int on = 4, name = 11, dev = 15, lis = 22, prt = 6;
};
Columns columnsFor(int width)
{
    Columns c;
    if (width < 40) {
        c.device = c.listen = c.port = false;
        c.name = std::max(6, width / 3);
    } else if (width < 60) {
        c.listen = false;
        c.port = true;
        c.name = 8;
        c.dev = 10;
        c.prt = 5;
    }
    return c;
}

Line tableHeader(const Columns& c, int width, bool utf8)
{
    std::string t = fit("On", c.on, utf8) + fit("Name", c.name, utf8);
    if (c.device) t += fit("Device", c.dev, utf8);
    if (c.listen) t += fit("Listen", c.lis, utf8);
    if (c.port) t += fit("Port", c.prt, utf8);
    t += "State";
    return plain(fit(t, width, utf8), Role::Muted);
}

Line portRow(const Painter& pt, const TuiState& s, const Columns& c, const PortMapping& p, int width, bool selected)
{
    // Text fields may end in one '\n' (BridgeConfig.h): never print it.
    auto clean = [](QString v) { return s8(v.remove(QLatin1Char('\n')).remove(QLatin1Char('\r'))); };
    std::string t = fit(pt.check(p.enabled) + " ", c.on, s.utf8) + fit(clean(p.displayName()), c.name - 1, s.utf8) + " ";
    if (c.device) t += fit(clean(p.comPort), c.dev - 1, s.utf8) + " ";
    if (c.listen) t += fit(clean(p.bindAddress) + ":" + std::to_string(p.tcpPort), c.lis - 1, s.utf8) + " ";
    if (c.port) t += fit(std::to_string(p.tcpPort), c.prt - 1, s.utf8) + " ";
    Line l{{t, Role::Ink}};
    for (Span& sp : stateCell(pt, s, p)) l.push_back(sp);
    if (selected) {
        // Reverse video marks the selection in every colour mode (allowed under NO_COLOR).
        int used = 0;
        for (Span& sp : l) used += cells(sp.text);
        if (used < width) l.push_back(Span{std::string(std::size_t(width - used), ' '), std::nullopt});
        for (Span& sp : l) sp.inverted = true;
    }
    return l;
}

// The service: `prefix`, a dot and the state, wrapped under the prefix so a long state never runs off the screen
// (user report 2026-10-20).
std::vector<Line> serviceLines(const Painter& pt, const TuiState& s, Line prefix, int width)
{
    std::string dot = pt.dotHollow();
    Role r = Role::Muted;
    if (s.serviceTone == Tone::Good) {
        dot = pt.dotFilled();
        r = Role::Teal;
    } else if (s.serviceTone == Tone::Bad) {
        dot = pt.dotError();
        r = Role::Danger;
    } else if (s.serviceTone == Tone::Warn) {
        r = Role::Warning;
    }
    prefix.push_back(Span{dot, r});
    prefix.push_back(Span{" ", std::nullopt});
    int indent = 0;
    for (const Span& sp : prefix) indent += cells(sp.text);
    // A state that fits stays beside the prefix; a longer one, where the indent leaves under 20 cells, starts on the
    // next line at the full width (a column of two-word lines helps nobody).
    const bool below = cells(s8(s.serviceLine)) > width - indent && width - indent < 20;
    const int textWidth = below ? width : width - indent;
    std::vector<Line> out;
    const std::vector<std::string> parts = wrap(s8(s.serviceLine), std::max(1, textWidth));
    if (below) out.push_back(prefix);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i == 0 && !below) {
            Line l = prefix;
            l.push_back(Span{parts[i], Role::Ink});
            out.push_back(l);
        } else {
            out.push_back(Line{{std::string(std::size_t(below ? 0 : indent), ' ') + parts[i], Role::Ink}});
        }
    }
    return out;
}

// The notes the header carries, most important first.
std::vector<std::pair<QString, Tone>> banners(const TuiState& s)
{
    std::vector<std::pair<QString, Tone>> out;
    if (!s.readOnlyReason.isEmpty()) out.emplace_back(s.readOnlyReason, Tone::Warn);
    if (s.otherBuild) {
        // How to update it, or (elevated) why u cannot yet: never "Press u" when u would refuse.
        const QString command = elevatedCommand(s);
        const QString note =
            !s.elevated && command.isEmpty() ? QString::fromLatin1(text::kOtherBuildNoteWindowOnly)
            : !s.elevated ? QString::fromLatin1(s.windows ? text::kOtherBuildNoteWindows : text::kOtherBuildNoteLinux)
                              .arg(command)
            : (s.load != Load::Ok && s.load != Load::NotCreated) ? QString::fromLatin1(text::kOtherBuildNoteUnloaded)
            : (!s.autoStart || !s.autoStartKnown)               ? QString::fromLatin1(text::kOtherBuildNoteAutoStart)
                                                                : QString::fromLatin1(text::kOtherBuildNote);
        out.emplace_back(note, Tone::Warn);
    }
    if (s.load == Load::Untrusted)
        out.emplace_back(s.elevated ? QString::fromLatin1(text::kUntrusted) + QStringLiteral(" Press R to repair it.")
                                    : QString::fromLatin1(text::kUntrusted),
                         Tone::Bad);
    if (s.load == Load::Error) out.emplace_back(s.loadError, Tone::Bad);
    if (s.pendingPresent) out.emplace_back(QString::fromLatin1(text::kPendingNote), Tone::Warn);
    if (s.disabledNote) out.emplace_back(QString::fromLatin1(text::kDisabledNote), Tone::Warn);
    if (s.busy) out.emplace_back(QString::fromUtf8(s.utf8 ? "Applying… " : "Applying... ") + s.busyStep, Tone::Plain);
    if (!s.interim.isEmpty()) out.emplace_back(s.interim, Tone::Warn);
    return out;
}

std::vector<Line> headerPanel(const Painter& pt, const TuiState& s, int width, int maxLines)
{
    std::vector<Line> lines;
    lines.push_back(plain(s8(QString::fromLatin1(text::kTitle)), Role::Cyan, true));
    std::vector<Line> notes;
    for (Line& l : serviceLines(pt, s, Line{{"Service ", Role::Muted, true}}, width)) notes.push_back(l);
    for (const auto& [t, tone] : banners(s)) para(notes, t, width, toneRole(tone));
    std::vector<Line> subtitle;
    para(subtitle, QString::fromLatin1(text::kSubtitle), width, Role::Muted);
    // The subtitle and a blank line only when everything else fits.
    if (int(lines.size() + subtitle.size() + 1 + notes.size()) <= maxLines) {
        for (Line& l : subtitle) lines.push_back(l);
        lines.push_back(plain(""));
    }
    for (Line& l : notes) lines.push_back(l);
    if (int(lines.size()) > maxLines) lines.resize(std::size_t(maxLines));
    return lines;
}

// ---- screens ---------------------------------------------------------------------------------------------
// `top` then `bottom`, exactly `height` lines: the bottom lines (the keys that answer the screen, and what it must
// say) always stay; when the top does not fit, its last kept line ends in an ellipsis (a long question
// at a small size pushed "y Yes  n No" off the screen).
std::vector<Line> pinBottom(std::vector<Line> top, const std::vector<Line>& bottom, int width, int height, bool utf8)
{
    const int room = std::max(0, height - int(bottom.size()));
    if (int(top.size()) > room) {
        top.resize(std::size_t(room));
        if (room > 0) {
            std::string text;
            for (const Span& sp : top.back()) text += sp.text;
            while (!text.empty() && text.back() == ' ') text.pop_back();
            const std::string more = utf8 ? "\u2026" : "~";
            top.back() = Line{{cells(text) + 1 <= width ? text + more : fit(text, width, utf8), Role::Muted}};
        }
    }
    while (int(top.size()) < room) top.push_back(Line{{"", std::nullopt}});
    for (const Line& l : bottom) top.push_back(l);
    if (int(top.size()) > height) top.erase(top.begin(), top.begin() + (int(top.size()) - height));
    return top;
}

// The operation finished while a form or a question was open: its result waits for the ports screen.
QString pendingNote(const TuiState& s)
{
    return QStringLiteral("Finished: %1. The result is shown when you are back on the ports screen.").arg(s.resultPending);
}

std::vector<Line> keysLine(const TuiState& s, int width)
{
    QString keys;
    if (width >= 100)
        keys = QStringLiteral("Enter edit  a add  d delete  Space on/off  s Save & apply  r reload  t start  x stop  p ports  l log  ? help  q quit");
    else if (width >= 60)
        keys = QStringLiteral("Enter edit  a add  d del  Space on/off  s save&apply  ? help  q quit");
    else
        keys = QStringLiteral("? keys  s save&apply  q quit");
    (void)s;
    return {plain(s8(keys), Role::Muted)};
}

Line footer(const TuiState& s)
{
    if (!s.message.isEmpty()) return plain(s8(s.message), toneRole(s.messageTone));
    if (s.dirty) return plain(s8(QString::fromLatin1(text::kUnsaved)), Role::Warning);
    if ((s.load == Load::Ok || s.load == Load::NotCreated) && s.work.ports.isEmpty())
        return plain(s8(QString::fromLatin1(text::kNoPorts)), Role::Muted);
    return plain("");
}

std::vector<Line> portsScreen(const Painter& pt, const TuiState& s, int width, int height, int* logoRows)
{
    std::vector<Line> out;
    *logoRows = 0;
    const LogoSize logo = logoSize(s.color, s.utf8);
    const bool showLogo = width >= 60 && height >= logo.height + 8;
    if (showLogo) {
        // The rows beside the logo; the caller draws the logo in the first logo.width columns.
        for (Line& l : headerPanel(pt, s, width - logo.width - 2, logo.height)) out.push_back(l);
        while (int(out.size()) < logo.height) out.push_back(plain(""));
        *logoRows = logo.height;
    } else {
        if (height >= 12) {
            Line h{{s8(QString::fromLatin1(text::kTitle)), Role::Cyan, true}, {" · ", Role::Muted}};
            if (!s.utf8) h[1].text = " - ";
            // "Service " is implied after the title.
            for (Line& l : serviceLines(pt, s, h, width)) out.push_back(l);
        }
        for (const auto& [t, tone] : banners(s)) para(out, t, width, toneRole(tone));
    }

    // The table, the auto-start row, the footer and the keys.
    const Columns c = columnsFor(width);
    const int fixedBelow = 1 /*auto-start*/ + 1 /*footer*/ + 1 /*keys*/;
    const int n = int(s.work.ports.size());
    // Stacked banners never push the whole table off a short screen: up to three port rows (the selected one among
    // them) keep their place, and the least important banner lines (the last ones) give way.
    const int keepRows = std::clamp(n, 1, 3);
    const int maxHeader = height - 1 /*table header*/ - fixedBelow - keepRows;
    if (!showLogo && maxHeader >= 0 && int(out.size()) > maxHeader) out.resize(std::size_t(maxHeader));
    int room = height - int(out.size()) - 1 /*table header*/ - fixedBelow;
    out.push_back(tableHeader(c, width, s.utf8));
    // Scroll so the selected port is always drawn: Enter and d act on the row the user sees.
    int top = 0;
    if (room > 0 && s.selected < n && s.selected >= room) top = s.selected - room + 1;
    const int shown = std::max(0, std::min(room, n - top));
    for (int i = top; i < top + shown; ++i) out.push_back(portRow(pt, s, c, s.work.ports[i], width, i == s.selected));
    for (int i = shown; i < room; ++i) out.push_back(plain(""));
    Line as{{(s.autoStartKnown ? pt.check(s.autoStart) : std::string("[?]")) + " " + s8(autoStartLabel(s.windows)) +
                 (s.autoStartKnown ? std::string() : std::string(" (not known)")),
             Role::Ink}};
    if (s.selected == n) {
        as.push_back(Span{std::string(std::size_t(std::max(0, width - cells(as[0].text))), ' '), std::nullopt});
        for (Span& sp : as) sp.inverted = true;
    }
    out.push_back(as);
    out.push_back(footer(s));
    for (Line& l : keysLine(s, width)) out.push_back(l);
    return out;
}

std::vector<Line> editScreen(const Painter& pt, const TuiState& s, int width, int height)
{
    const EditForm& f = s.form;
    const int label = width < 40 ? 11 : 13;
    std::vector<Line> fields;
    for (int i = 0; i < kFieldCount; ++i) {
        const Field fld = Field(i);
        std::string value;
        if (fld == Field::Enabled) value = pt.check(f.enabled);
        else if (fld == Field::Dtr) value = pt.check(f.dtr);
        else if (fld == Field::Rts) value = pt.check(f.rts);
        else value = s8(f.text[i]) + (i == f.focus ? "_" : "");
        Line l{{fit(s8(fieldLabel(fld)), label, s.utf8), Role::Muted}};
        const int avail = std::max(1, width - label);
        // The field being typed in shows its end: a long value scrolls left, with an ellipsis where it was cut, so
        // what is typed and the cursor stay visible. Other fields show their start.
        Span v{i == f.focus ? tailFit(value, avail, s.utf8) : value, Role::Ink, false, i == f.focus};
        l.push_back(v);
        fields.push_back(l);
    }
    // The footer is reserved first: the keys, then why Enter was refused (the field error, or the read-only reason),
    // then the notices while they fit, so on a short screen the error is the last to go. One row stays
    // for the field.
    const std::string keys = width >= 60 ? "Tab next  Shift+Tab back  Space toggle  Enter accept  Esc cancel"
                                         : "Tab next  Enter accept  Esc cancel";
    int footerLeft = std::max(1, height - 1) - 1;   // after the keys
    std::vector<Line> error, message, pending;
    if (!f.error.isEmpty()) para(error, f.error, width, Role::Danger);
    else if (!s.elevated) para(error, s.readOnlyReason, width, Role::Warning);
    if (!s.message.isEmpty()) para(message, s.message, width, toneRole(s.messageTone));
    if (!s.resultPending.isEmpty()) para(pending, pendingNote(s), width, Role::Warning);
    auto fits = [&footerLeft](std::vector<Line>& lines) {
        if (int(lines.size()) > footerLeft) {
            // The error is cut to what fits rather than dropped; the notices go whole or not at all.
            return false;
        }
        footerLeft -= int(lines.size());
        return true;
    };
    if (int(error.size()) > footerLeft) error.resize(std::size_t(std::max(0, footerLeft)));
    fits(error);
    if (!fits(message)) message.clear();
    if (!fits(pending)) pending.clear();
    std::vector<Line> bottom;
    for (const Line& l : error) bottom.push_back(l);
    for (const Line& l : message) bottom.push_back(l);
    for (const Line& l : pending) bottom.push_back(l);
    bottom.push_back(plain(keys, Role::Muted));
    const int room = std::max(0, height - int(bottom.size()));

    // What the focused field means (owner request 2026-10-19), under the form. It only takes rows the whole form and
    // its headings do not need, so it is the first thing to go on a short screen: cut to the rows left, its last
    // line ending in an ellipsis, and gone when the form itself does not fit. A blank line sets it off when it fits.
    std::vector<Line> hint;
    const int spare = room - (kFieldCount + 2);
    if (spare > 0) {
        for (QString entry : fieldHint(Field(f.focus), s.windows)) {
            if (!s.utf8) entry.replace(QChar(0x00B7), QLatin1Char('-'));   // the Windows Allowed example line's dot
            for (const std::string& l : wrap(s8(entry), width)) hint.push_back(plain(l, Role::Muted));
        }
        if (int(hint.size()) > spare) {
            hint.resize(std::size_t(spare));
            std::string last = hint.back().front().text;
            const std::string more = s.utf8 ? "\u2026" : "~";
            hint.back() = plain(cells(last) + 1 <= width ? last + more : fit(last, width, s.utf8), Role::Muted);
        }
        if (int(hint.size()) < spare) hint.insert(hint.begin(), plain(""));
    }

    // Then the fields: all of them with the headings when they fit; otherwise the headings go and a window of fields
    // around the focused one is drawn, so the field being typed in is always on the screen.
    std::vector<Line> out;
    const bool headings = room >= kFieldCount + 2;
    if (headings) {
        const QString title = f.index < 0 ? QStringLiteral("New port")
                                          : QStringLiteral("Edit port %1").arg(f.text[int(Field::Name)]);
        out.push_back(plain(s8(title), Role::Cyan, true));
        out.push_back(plain(pt.rule(width), Role::Muted));
    }
    const int rows = std::clamp(room - int(out.size()), std::min(1, room), kFieldCount);
    const int start = std::clamp(f.focus - rows / 2, 0, kFieldCount - rows);
    for (int i = start; i < start + rows; ++i) out.push_back(fields[std::size_t(i)]);
    for (const Line& l : hint) out.push_back(l);
    while (int(out.size()) < room) out.push_back(plain(""));
    for (const Line& l : bottom) out.push_back(l);
    return out;
}

std::vector<Line> confirmScreen(const Painter&, const TuiState& s, int width, int height)
{
    // While this screen is up, y authorises the decision, so the decision is what must be on it. The
    // answer line comes first, then the question: whole when it fits, else its one-sentence decision, else the end of
    // that sentence with an ellipsis. Notices (a message, a parked result), the spacer and the title only take the
    // rows that are left, in that order.
    // Too small to show the decision: y is not offered (and the controller ignores it), only No.
    const bool canAnswer = decisionVisible(s, width, height);
    // Where No acts too (the build question), Esc is the separate way out; elsewhere No and Esc both cancel.
    Line answer;
    if (s.questionNoActs)
        answer = canAnswer ? Line{{"y", Role::Cyan, true}, {" Yes  ", Role::Ink}, {"n", Role::Cyan, true}, {" No  ", Role::Ink},
                                  {"Esc", Role::Cyan, true}, {" cancel", Role::Ink}}
                           : Line{{"Too small: ", Role::Warning}, {"Esc", Role::Cyan, true}, {" cancels", Role::Ink}};
    else
        answer = canAnswer ? Line{{"y", Role::Cyan, true}, {" Yes    ", Role::Ink}, {"n", Role::Cyan, true}, {" No (Esc)", Role::Ink}}
                           : Line{{"Too small to answer: ", Role::Warning}, {"n", Role::Cyan, true}, {" No", Role::Ink}};
    const int room = std::max(0, height - 1);
    std::vector<Line> question;
    para(question, s.question, width, Role::Ink);
    if (int(question.size()) > room && !s.questionShort.isEmpty()) {
        question.clear();
        para(question, s.questionShort, width, Role::Ink);
    }
    if (int(question.size()) > room) {
        question.erase(question.begin(), question.end() - room);
        if (room > 0) {
            std::string text;
            for (const Span& sp : question.front()) text += sp.text;
            question.front() = Line{{(s.utf8 ? "\u2026" : "~") + text, Role::Ink}};
        }
    }
    int left = room - int(question.size());
    auto take = [&left](const std::vector<Line>& lines) {
        if (int(lines.size()) > left) return std::vector<Line>{};
        left -= int(lines.size());
        return lines;
    };
    std::vector<Line> message, pending;
    if (!s.message.isEmpty()) para(message, s.message, width, toneRole(s.messageTone));
    if (!s.resultPending.isEmpty()) para(pending, pendingNote(s), width, Role::Warning);
    message = take(message);
    pending = take(pending);
    const std::vector<Line> spacer = take({plain("")});
    const std::vector<Line> title = take({plain("Confirm", Role::Warning, true), plain("")});

    std::vector<Line> out = title;
    for (Line& l : question) out.push_back(l);
    for (const Line& l : message) out.push_back(l);
    for (const Line& l : pending) out.push_back(l);
    // With a title the answer sits below the question; without one (a short screen) it is the last line.
    if (title.empty())
        while (int(out.size() + spacer.size()) < room) out.push_back(plain(""));
    for (const Line& l : spacer) out.push_back(l);
    if (height > 0) out.push_back(answer);
    while (int(out.size()) < height) out.push_back(plain(""));
    return out;
}

std::vector<Line> resultScreen(const Painter&, const TuiState& s, int width, int height)
{
    std::vector<Line> out;
    out.push_back(plain(s8(s.resultTitle), toneRole(s.resultTone), true));
    out.push_back(plain(""));
    for (const QString& l : s.resultLines) para(out, l, width, Role::Ink);
    std::vector<Line> bottom{plain(""), Line{{"Enter", Role::Cyan, true}, {" continue", Role::Ink}}};
    if (int(out.size() + bottom.size()) <= height) {
        for (const Line& l : bottom) out.push_back(l);
        while (int(out.size()) < height) out.push_back(plain(""));
        return out;
    }
    return pinBottom(std::move(out), bottom, width, height, s.utf8);
}

std::vector<Line> pageScreen(const Painter& pt, const TuiState& s, int width, int height)
{
    std::vector<Line> out;
    out.push_back(plain(s8(s.pageTitle), Role::Cyan, true));
    std::vector<Line> body;
    for (int i = 0; i < int(s.pageLines.size()); ++i) {
        const QString& l = s.pageLines[i];
        if (l.isEmpty()) body.push_back(plain(""));
        else if (s.pageHeadings.contains(i)) para(body, l, width, Role::Cyan, true);
        else para(body, l, width, Role::Ink);
    }
    const int room = std::max(0, height - 2);
    // The same bound the controller keeps pageTop in (pageMaxTop): the last page is full, never one line.
    const int top = std::clamp(s.pageTop, 0, std::max(0, int(body.size()) - room));
    for (int i = top; i < int(body.size()) && int(out.size()) < room + 1; ++i) out.push_back(body[std::size_t(i)]);
    while (int(out.size()) < height - 1) out.push_back(plain(""));
    out.push_back(plain(s.utf8 ? "↑↓ PgUp PgDn scroll  Esc back" : "Up/Down PgUp PgDn scroll  Esc back", Role::Muted));
    (void)pt;
    return out;
}

}  // namespace

std::vector<std::string> wrap(const std::string& text, int width)
{
    std::vector<std::string> out;
    if (width <= 0) return out;
    std::string line;
    int used = 0;
    std::string word;
    int wordW = 0;
    auto flushWord = [&] {
        if (word.empty()) return;
        if (used > 0 && used + 1 + wordW > width) {
            out.push_back(line);
            line.clear();
            used = 0;
        }
        if (used > 0) {
            line += ' ';
            used += 1;
        }
        // A word longer than the line is cut.
        for (const std::string& g : Utf8ToGlyphs(word)) {
            if (g.empty()) continue;
            const int w = string_width(g);
            if (used + w > width) {
                out.push_back(line);
                line.clear();
                used = 0;
            }
            line += g;
            used += w;
        }
        word.clear();
        wordW = 0;
    };
    for (const std::string& g : Utf8ToGlyphs(text)) {
        if (g.empty()) continue;
        if (g == " " || g == "\n") {
            flushWord();
            if (g == "\n") {
                out.push_back(line);
                line.clear();
                used = 0;
            }
            continue;
        }
        word += g;
        wordW += string_width(g);
    }
    flushWord();
    if (!line.empty() || out.empty()) out.push_back(line);
    return out;
}

bool decisionVisible(const TuiState& s, int width, int height)
{
    const int room = height - 1;   // the answer line
    if (room <= 0) return false;
    if (int(wrap(s8(s.question), std::max(1, width)).size()) <= room) return true;
    return !s.questionShort.isEmpty() && int(wrap(s8(s.questionShort), std::max(1, width)).size()) <= room;
}

int pageMaxTop(const TuiState& s, int width, int height)
{
    width = std::max(1, width);
    int lines = 0;
    for (const QString& l : s.pageLines) lines += l.isEmpty() ? 1 : int(wrap(s8(l), width).size());
    return std::max(0, lines - std::max(0, height - 2));
}

Element render(const TuiState& s, int width, int height)
{
    width = std::max(1, width);
    height = std::max(1, height);
    const Painter pt(s);
    std::vector<Line> lines;
    int logoRows = 0;
    switch (s.screen) {
    case Screen::Ports: lines = portsScreen(pt, s, width, height, &logoRows); break;
    case Screen::Edit: lines = editScreen(pt, s, width, height); break;
    case Screen::Confirm: lines = confirmScreen(pt, s, width, height); break;
    case Screen::Result: lines = resultScreen(pt, s, width, height); break;
    case Screen::Page: lines = pageScreen(pt, s, width, height); break;
    }
    if (int(lines.size()) > height) lines.resize(std::size_t(height));
    while (int(lines.size()) < height) lines.push_back(plain(""));

    Elements rows;
    if (logoRows > 0) {
        const LogoSize logo = logoSize(s.color, s.utf8);
        Elements right;
        for (int i = 0; i < logoRows; ++i) right.push_back(pt.line(lines[std::size_t(i)], width - logo.width - 2));
        Element logoBlock = logoElement(s.color, s.utf8);
        // The logo is centred in its column when the ASCII ramp is narrower than the block art.
        rows.push_back(hbox({logoBlock | size(WIDTH, EQUAL, logo.width) | size(HEIGHT, EQUAL, logoRows),
                             ftxui::text("  "), vbox(std::move(right))}));
    }
    for (int i = logoRows; i < height; ++i) rows.push_back(pt.line(lines[std::size_t(i)], width));
    Element root = vbox(std::move(rows)) | size(WIDTH, EQUAL, width) | size(HEIGHT, EQUAL, height);
    if (const std::optional<Color> bg = colorFor(Role::Bg, s.color)) root = root | bgcolor(*bg);
    if (const std::optional<Color> ink = colorFor(Role::Ink, s.color)) root = root | color(*ink);
    return root;
}

std::vector<std::string> renderText(const TuiState& s, int width, int height)
{
    ftxui::Screen screen(width, height);
    Render(screen, render(s, width, height));
    std::vector<std::string> out;
    for (int y = 0; y < height; ++y) {
        std::string line;
        for (int x = 0; x < width; ++x) {
            const std::string& c = screen.CellAt(x, y).character;
            line += c.empty() ? std::string(" ") : c;
        }
        out.push_back(line);
    }
    return out;
}

}  // namespace asb::tui
