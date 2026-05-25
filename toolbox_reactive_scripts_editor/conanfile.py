from conan import ConanFile


class ToolboxReactiveScriptsEditorConan(ConanFile):
    name = "toolbox_reactive_scripts_editor"
    version = "0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    requires = (
        "plotjuggler_core/0.2.1",
        "gtest/1.17.0",
        "lua/5.4.6",
        "sol2/3.5.0",
        "nlohmann_json/3.12.0",
        "pybind11/2.13.6",
        "cpython/3.12.7",
    )
    default_options = {
        "*:shared": False,
        "lua/*:compile_as_cpp": True,
        "cpython/*:shared": False,
        "cpython/*:with_tkinter": False,
    }
