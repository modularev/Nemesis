# faust_build.py by Arev Imer
# builds faust dsp file during platform.io build task

import subprocess
# Import("env")
# env.Append(LINKFLAGS=["-specs=nosys.specs"])

buildLib = 'rm -r lib; for i in src/*.dsp; do faust2nemesis -hr -ext -double  "$i"; done'
# -ext statement for accessing PSRAM on teensy 4.1 not available in official faust release (status: feb '21)

subprocess.run(buildLib, shell=True)