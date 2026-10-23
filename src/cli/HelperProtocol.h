#pragma once

// The Linux pkexec helper's wire format (WS-9 "Outputs"; master §5.7, D2, D34; INV-89). Shared by the helper
// (`aether-serial-bridge --helper`, Helper.h) and its client (HelperClient.h, the Linux GUI's side).
//
// Request: exactly one JSON object on stdin, at most kMaxRequestBytes in total, then EOF.
//   {"op":"save-and-apply","configBytes":"<base64 of the exact bytes>",
//    "source":{"kind":"pending|committed|new","sha256":"<hex>"},"supersedePending":bool}
//   {"op":"deactivate","configBytes":"…"?,"source":…?,"supersedePending":bool?}   (source only with configBytes)
//   {"op":"start"|"stop"|"repair"|"discard-pending"|"inspect"}
// v1.1 (WS-15, INV-123; the v1.0 ops above, their members and their responses are frozen byte for byte):
//   {"op":"install","configBytes":"…"?,"source":…?,"supersedePending":bool?}  (as deactivate: source only with configBytes)
//   {"op":"uninstall","removeSettings":bool}                                   (required)
//   {"op":"discard-pending-if","sha256":"<64 lower-case hex>"}                 (required)
// install and uninstall run only in a staged program (Helper.h); an installed helper refuses them.
// Strict: unknown ops and members, a member given twice, wrong types, base64 that is not exact, a sha256 that is
// not 64 lower-case hex digits, and anything after the value but whitespace are refused.
//
// Response: one JSON object on stdout.
//   {"ok":bool,"outcome":"…","persisted":"None|Staged|Committed","persistedSha256":"<hex>"?,"step":"…"?,
//    "message":"…","postcondition":"…"?,"fileChanges":[…]}
//   inspect: {"ok":true,"trust":"Trust|Create|Recreate|Refuse","reason":str,"installation":"…","service":"…"}
// Digest convention: persistedSha256 is the sha256 of the exact bytes carried in configBytes (decoded); the helper
// never reserialises them (they are the SaveRequest's config, byte for byte).

#include "core/Interfaces.h"

#include <QByteArray>
#include <QString>

#include <chrono>
#include <optional>

namespace asb::cli::helper {

// D34: the request's cumulative size limit and the deadline for its EOF.
inline constexpr qsizetype kMaxRequestBytes = 262144;
inline constexpr std::chrono::seconds kInputDeadline{10};

enum class Op { SaveAndApply, Deactivate, Start, Stop, Repair, DiscardPending, Inspect, Install, Uninstall, DiscardPendingIf };
QString opName(Op);   // the wire name: "save-and-apply", ...

struct Request {
    Request() = default;
    Request(Op o, std::optional<SaveRequest> s) : op(o), save(std::move(s)) {}
    Op op = Op::Inspect;
    // save-and-apply: always; deactivate and install: when it carries configBytes (the edits to keep as pending, the
    // settings to install with).
    std::optional<SaveRequest> save;
    bool removeSettings = false;   // uninstall
    QString sha256;                // discard-pending-if: the digest of the pending bytes the window showed
};

// inspect's answer: the data folder's ReadOnly trust decision and the service as the helper (root) sees it.
struct InspectReport {
    QString trust;          // Trust | Create | Recreate | Refuse
    QString reason;         // why the folder is not trusted (empty for Trust/Create)
    QString installation;   // NotInstalled | IncompleteInstall | Current | Foreign | Unknown
    QString service;        // Absent | Stopped | StartPending | Running | StopPending | Other | Unknown
};

// The names the wire uses for the core enums.
QString trustName(DirAction);
QString installationName(InstallState);
QString serviceName(ServiceQuery::State);

// ---- the client's side --------------------------------------------------------------------------------
QByteArray encodeRequest(const Request&);
// A response to a mutating op; nullopt (with *error) when it is not one. `ok` is returned through *ok.
std::optional<Result> parseResultResponse(const QByteArray& bytes, bool* ok, QString* error);
std::optional<InspectReport> parseInspectResponse(const QByteArray& bytes, bool* ok, QString* error);

// ---- the helper's side --------------------------------------------------------------------------------
// Exactly one JSON object (whitespace around it only), validated as above; nullopt with *error otherwise.
std::optional<Request> parseRequest(const QByteArray& bytes, QString* error);
// `ok` is the CLI's success rule (Ok or SavedNotApplied).
QByteArray encodeResult(const Result&);
QByteArray encodeInspect(const InspectReport&, bool ok);
bool succeeded(const Result&);

}  // namespace asb::cli::helper
