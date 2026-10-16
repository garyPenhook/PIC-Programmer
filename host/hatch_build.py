"""Build hook: put the shared device table into the package.

data/pic_devices.csv and tools/gen_devices.py live at the repository root because the
firmware build uses them too. When building from a checkout, copy them into
picprog/data/ so an installed picprog has them. An sdist already contains those copies,
so a wheel built from it needs nothing more.
"""
import os

from hatchling.builders.hooks.plugin.interface import BuildHookInterface

SHARED = {
    os.path.join("..", "data", "pic_devices.csv"): "picprog/data/pic_devices.csv",
    os.path.join("..", "tools", "gen_devices.py"): "picprog/data/gen_devices.py",
}


class CustomBuildHook(BuildHookInterface):
    def initialize(self, version, build_data):
        for source, target in SHARED.items():
            path = os.path.normpath(os.path.join(self.root, source))
            if os.path.exists(path):
                build_data["force_include"][path] = target
