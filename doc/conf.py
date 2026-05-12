"""Sphinx configuration for the HiSPEC-TIB firmware documentation."""

from pathlib import Path
import shutil
import subprocess

from sphinx.errors import ExtensionError


DOC_DIR = Path(__file__).resolve().parent
DOXYGEN_XML = DOC_DIR / "_build_doxygen" / "xml"

project = "HiSPEC-FIB Firmware"
author = "Caltech Optical Observatories"
release = "0.1"

extensions = [
    "myst_parser",
    "breathe",
    "sphinxcontrib.mermaid",
]

source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

templates_path = ["_templates"]
exclude_patterns = [
    "_build_sphinx",
    "_build_doxygen",
    "_doxygen",
    "Thumbs.db",
    ".DS_Store",
]

html_theme = "alabaster"
html_theme_options = {
    "description": "Zephyr RTOS firmware documentation",
    "fixed_sidebar": True,
}
html_sidebars = {
    "**": [
        "about.html",
        "navigation.html",
        "localtoc.html",
        "relations.html",
        "searchbox.html",
    ]
}

myst_heading_anchors = 3
myst_fence_as_directive = ["mermaid"]

breathe_projects = {
    "hispec_tib": str(DOXYGEN_XML),
}
breathe_default_project = "hispec_tib"

def run_doxygen(_app):
    """Generate Doxygen XML before Breathe resolves API directives."""

    if shutil.which("doxygen") is None:
        raise ExtensionError(
            "Doxygen is required to build the API reference. Install doxygen "
            "and rerun the Sphinx build."
        )

    try:
        subprocess.run(["doxygen", "Doxyfile"], cwd=DOC_DIR, check=True)
    except subprocess.CalledProcessError as exc:
        raise ExtensionError(f"Doxygen failed with exit status {exc.returncode}") from exc


def setup(app):
    app.connect("builder-inited", run_doxygen)
    return {
        "version": "1.0",
        "parallel_read_safe": True,
        "parallel_write_safe": True,
    }
