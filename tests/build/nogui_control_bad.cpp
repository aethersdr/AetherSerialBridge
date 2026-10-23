// INV-37 control target. This program DELIBERATELY uses QtGui (QColor) and links Qt6::Gui.
// NoQtGuiCheck must reject it. It is built only so the check can be run against something that is
// known to be bad; nothing ever links or ships it.

#include <cstdio>

#include <QByteArray>
#include <QColor>

int main() {
    const QColor c(12, 34, 56);
    std::printf("%s\n", c.name().toLatin1().constData());
    return 0;
}
