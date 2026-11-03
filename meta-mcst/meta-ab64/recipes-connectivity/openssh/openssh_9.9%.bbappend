# no HostKey lines added: the recipe appends them to the end of sshd_config,
# which is inside our "Match LocalPort 2022" block there (sshd refuses
# HostKey in a Match); without them sshd offers its built-in default, all
# three keys (RSA, ECDSA, Ed25519), as 8.4 did
PACKAGECONFIG_remove = "hostkey-ecdsa"
