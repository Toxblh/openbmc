# The project renamed its default branch master -> main (the pinned SRCREV is
# unchanged); GitHub serves git only over https. See reimu-fetch-fixes.inc.
SRC_URI := "${@d.getVar('SRC_URI', False).replace('git://github.com/htop-dev/htop.git', 'git://github.com/htop-dev/htop.git;protocol=https;branch=main')}"
