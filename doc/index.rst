HiSPEC-FIB Firmware Documentation
=================================

Start with hardware to understand the PCBs, operation pages for algorithms and
measurements, or the interface specification to control a board. Python helpers
are an in-repository lab/test tool; the official Python API is external.

Hardware mappings and the control specification describe intended behavior.
Source describes the implementation. Unresolved differences remain explicit in
the human-review ledger.

.. toctree::
   :maxdepth: 2
   :caption: Hardware

   hardware_overview.md

.. toctree::
   :maxdepth: 2
   :caption: Operation and Mathematics

   mathematics.md
   mems_splitting.md
   photodiode_notes.md
   throughput.md
   attenuator.md
   attenuator_calibration.md
   laser_operation.md
   laser_calibration.md
   calibration_fitting.md

.. toctree::
   :maxdepth: 2
   :caption: Interfaces and Python

   commands.md
   python_helpers.md
   warnings_and_telemetry.md

.. toctree::
   :maxdepth: 2
   :caption: Firmware Maintenance

   ota.md

.. toctree::
   :maxdepth: 2
   :caption: Firmware Internals

   architecture.md
   settings.md
   api/index.md

.. toctree::
   :maxdepth: 1
   :caption: Evidence and Human Review

   temp_test_result.md
   human_review_required.md

* :ref:`genindex`
* :ref:`search`
