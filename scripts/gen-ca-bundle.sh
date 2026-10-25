#!/usr/bin/env bash
# Regenerates core/src/ca_bundle.inc from the system's Mozilla CA store.
set -euo pipefail
CERTS=${CERTS:-/etc/ssl/certs}
OUT=$(cd "$(dirname "$0")/.." && pwd)/core/src/ca_bundle.inc
{
  echo "// Root CAs used by Discord (Cloudflare/Google Trust Services, Let's Encrypt, DigiCert, ...)."
  echo "// Generated from the Mozilla CA bundle; regenerate with scripts/gen-ca-bundle.sh."
  echo "static const char kCaBundle[] ="
  for c in GTS_Root_R1 GTS_Root_R3 GTS_Root_R4 ISRG_Root_X1 ISRG_Root_X2 DigiCert_Global_Root_G2 \
           DigiCert_Global_Root_G3 GlobalSign_Root_CA_-_R3 USERTrust_RSA_Certification_Authority Amazon_Root_CA_1; do
    sed 's/.*/    "&\\n"/' "$CERTS/$c.pem"
  done
  echo "    ;"
} > "$OUT"
