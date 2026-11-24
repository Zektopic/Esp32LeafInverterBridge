# Reference implementation (firmware v1.3.0)

Unmodified copies of the original Arduino CAN handler (`can_bridge_manager_leaf.cpp`
and the headers it includes) taken from commit `43a861d`. They are **not** part of
the library or of either firmware build.

`../diff_original.cpp` compiles them on the host against the minimal Arduino /
ACAN2515 stubs in `stubs/` and replays long randomised drive traces through both
the original handler and `leaf_inverter_process()`, comparing every frame that
would be transmitted on each bus. This is the regression proof that the
extraction into `leaf_can` preserved the in-car behaviour; the intentional
differences are listed in `../diff_original.cpp`.
