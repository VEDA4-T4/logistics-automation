CREATE TABLE process_gripper_target (
    work_id TEXT PRIMARY KEY NOT NULL CHECK(length(work_id) > 0),
    x_mm REAL NOT NULL,
    y_mm REAL NOT NULL,
    z_mm REAL NOT NULL,
    yaw_deg REAL NOT NULL,
    box_length_mm REAL NOT NULL CHECK(box_length_mm > 0),
    box_width_mm REAL NOT NULL CHECK(box_width_mm > 0),
    box_height_mm REAL NOT NULL CHECK(box_height_mm > 0),
    coordinate_frame TEXT NOT NULL CHECK(length(coordinate_frame) > 0),
    calibration_version INTEGER NOT NULL CHECK(calibration_version > 0),
    updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms >= 0),
    FOREIGN KEY(work_id) REFERENCES process_work_state(work_id) ON DELETE CASCADE
);
