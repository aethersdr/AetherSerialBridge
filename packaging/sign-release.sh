#!/usr/bin/env bash
# The release signing hook (WS-13; master D6: v1 is UNSIGNED). release.yml calls it only when a maintainer sets the
# repository variable ASB_SIGN_RELEASE=true, and until signing exists it refuses, so turning the switch on without
# doing the work fails the release instead of shipping files that look signed.
#
# usage: packaging/sign-release.sh <release dir>   (the folder holding the artifacts and SHA256SUMS)
#
# Where signing goes when it is decided (each a new decision, with its keys in repository secrets):
#   Windows   Authenticode both .exe inside the zip (for example Azure Trusted Signing, as AetherSDR's
#             sign-release.yml), BEFORE the zip and SHA256SUMS are made, so this hook would move into the windows job;
#   Linux     a detached signature of SHA256SUMS (gpg --detach-sign --armor -> SHA256SUMS.asc) with a published key.
set -euo pipefail
dir="${1:?usage: sign-release.sh <release dir>}"
test -f "$dir/SHA256SUMS" || { echo "no SHA256SUMS in $dir" >&2; exit 2; }
echo "release signing is not configured (D6: v1 ships unsigned); unset ASB_SIGN_RELEASE or implement this hook" >&2
exit 1
