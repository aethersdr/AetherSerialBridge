# Polkit defaults on another distribution (v1.1 WS-15, R21): Fedora's polkit, its admin rule (unix-group:wheel) and its
# directory modes, and a real auth_admin prompt through pkexec + pkttyagent. No systemd: the systemd suite's driver
# starts dbus-daemon and polkitd by hand in this container (sysd.PolkitBox). Pinned by digest.
#
# The program under test and its Qt come in by bind mount, as in systemd-bookworm.Dockerfile. krb5-libs and libbrotli:
# the dynamic development build's QtNetwork needs them (the release program links Qt statically).
FROM fedora:41@sha256:f1a3fab47bcb3c3ddf3135d5ee7ba8b7b25f2e809a47440936212a3a50957f3d

RUN dnf -y install polkit dbus-daemon dbus-libs python3 procps-ng util-linux passwd krb5-libs libbrotli \
    && dnf clean all \
    # The harness's users: tuser (uid 1000) asks; tadmin is the administrator polkit's auth_admin asks for (Fedora's
    # admin rule: unix-group:wheel), with a fixed TEST password answered on a pseudo-terminal.
    && useradd -m -u 1000 tuser \
    && useradd -m -G wheel tadmin \
    && echo 'tadmin:asb-test-admin' | chpasswd

CMD ["sleep", "infinity"]
