# OpenSSL 3: its RSA_/EC_KEY_ key generation is deprecated there, not gone;
# the build has -Werror (no behaviour change)
CXXFLAGS_append = " -Wno-error=deprecated-declarations"
