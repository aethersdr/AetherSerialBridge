// The control of process_creation_audit (INV-112): an object that calls QProcess::start. The audit must find it in
// the object layer; never part of a product executable.
#include <QProcess>

int main()
{
    QProcess p;
    p.start(QStringLiteral("true"), {});
    return p.waitForFinished(1000) ? 0 : 1;
}
