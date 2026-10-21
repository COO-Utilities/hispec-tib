# Laser and Maiman C contracts

For sequencing, health, power estimates and tuning, see
[laser operation](../laser_operation.md). Compiled diode properties are defaults,
not new measurements of an installed diode.

```{eval-rst}
.. doxygenfile:: app/src/maiman.h
   :project: hispec_tib

.. doxygenfile:: app/src/lasers.h
   :project: hispec_tib

.. doxygenfile:: app/src/laser_properties.h
   :project: hispec_tib
   :sections: briefdescription detaileddescription define innerclass
```

The {download}`compiled property table <../../app/src/laser_properties.h>` contains
the per-diode initializers, vendor provenance and inferred-value comments.
