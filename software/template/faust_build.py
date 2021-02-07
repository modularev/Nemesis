# faust_build.py by Arev Imer
# builds faust dsp file during platform.io build task

import subprocess

buildLib = 'faust2teensy -lib -ext src/*.dsp' # -ext statement for accessing PSRAM on teensy 4.1 not available in official faust release (status: feb '21)
move = 'mv *.zip lib'
unpack = 'unzip -o lib/*.zip -d lib && rm lib/*.zip'

subprocess.run(buildLib, shell=True)
subprocess.run(move, shell=True)
subprocess.run(unpack, shell=True)