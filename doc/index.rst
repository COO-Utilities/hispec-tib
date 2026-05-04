HiSPEC-FIB Firmware Documentation
=================================

This documentation is built from Markdown-first architecture and audit pages
plus Doxygen-extracted C API reference material.

Source-of-truth rules:

* ``hardware.md`` is the hardware source of truth.
* ``commands.md`` is the intended command/API source of truth.
* Current C source is the implementation source of truth.
* Mismatches are centralized in ``human_review_required.md``.

.. toctree::
   :maxdepth: 2
   :caption: Architecture and Runtime

   architecture.md
   hardware_profiles.md
   threads.md
   queues_and_work.md
   settings.md
   warnings_and_telemetry.md
   diagrams.md

.. toctree::
   :maxdepth: 2
   :caption: Commands and Audit

   commands.md
   implemented_commands.md
   command_implementation_audit.md
   implementation_gaps.md
   human_review_required.md

.. toctree::
   :maxdepth: 2
   :caption: Developer Maintained Docs

   status.md
   libraries.md
   networking_plan.md
   nuisances.md
   hardware.md

.. toctree::
   :maxdepth: 2
   :caption: API Reference

   api/index.md

Indices and tables
==================

* :ref:`genindex`
* :ref:`search`
