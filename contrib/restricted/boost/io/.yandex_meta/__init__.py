from devtools.yamaker import boost
from devtools.yamaker.project import NixSourceProject


def post_install(self):
    self.yamakes["."] = boost.make_library(self)


boost_io = NixSourceProject(
    nixattr="boost_io",
    arcdir=boost.make_arcdir("io"),
    owners=["g:cpp-contrib", "g:taxi-common"],
    copy_sources=[
        "include/boost/",
    ],
    post_install=post_install,
)
