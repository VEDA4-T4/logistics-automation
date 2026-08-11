# Qt Control Center Factory Top-View Design

## Goal

Replace the card-only process overview with a compact live factory top-view while retaining the existing process cards for exact state, work, sensor, and control-target details. Polish the surrounding control-center UI without changing MQTT contracts or process-control behavior.

## Scope

This work covers the Qt control-center only:

- live top-view rendering for input, vision, gripper, sorting, and line-tracer nodes;
- the approved equipment layout and animations;
- existing process cards retained to the right of the drawing;
- selection synchronization between cards and drawing;
- consistent control-center spacing, colors, typography, tabs, buttons, scrollbars, and surfaces;
- tests for presentation mapping and interaction.

It does not change MQTT topics or payloads, central-server orchestration, device-node behavior, RTSP playback, or command semantics.

## Approved Layout

The application keeps its existing header and lower monitoring workspace. The current fixed-height card strip becomes a resizable operations overview above the video/detail workspace.

- A vertical `QSplitter` separates the operations overview from the existing video/detail workspace.
- The overview starts near 300 px high, with a 250 px practical minimum.
- The lower video/detail workspace keeps a minimum usable height so the camera grid does not collapse.
- Inside the overview, the top-view occupies the left side.
- The existing overall and per-node cards occupy the right side in a compact two-column grid.
- The right card area may scroll vertically when content cannot fit; the drawing does not shrink to accommodate card overflow.
- The existing horizontal video/detail splitter and its user-adjustable proportions remain intact.

This layout is optimized for the application's existing 1280x720 minimum and 1600x900 default window sizes. A separate responsive bottom-card layout is not required.

## Component Boundaries

### FactoryTopViewWidget

Add one focused Qt widget backed by `QGraphicsView` and `QGraphicsScene`. It owns only drawing, animation, hit detection, and conversion from dashboard presentation state to scene properties.

The widget receives the existing `OperationsDashboardState` output. It does not parse MQTT JSON and does not own process truth. Scene items are created once and updated in place.

It exposes process selection using the existing process key/device ID mapping. Selecting equipment emits the same control-target selection used by the cards.

### OperationsDashboardPanel

Retain the existing card creation and refresh behavior. Reorganize the panel into the approved left drawing/right card layout and forward state updates to `FactoryTopViewWidget`.

Card contents remain:

- overall and per-node state;
- work ID or error/warning detail;
- sensor state and distance in centimeters;
- device ID and last update time;
- click-to-select control target.

Card selection and drawing selection must remain synchronized in both directions.

### MainWindow

Replace the fixed top-strip placement with the vertical splitter. MQTT message handling continues to update `OperationsDashboardState` once, then passes the resulting state to the composite dashboard panel as it does today.

## Drawing Geometry

The approved schematic is a simple dark top-view based on the supplied reference image:

- the input conveyor runs horizontally across the top;
- the sorting conveyor runs vertically on the right and is the same represented length;
- the vision camera and input ultrasonic sensor share the recognition position;
- the gripper pivot sits at the virtual intersection of the input and sorting conveyor axes without extending either conveyor;
- the gripper rotates 90 degrees between the input conveyor end to the west and the sorting conveyor end to the south;
- the sorting servo sits between line-tracer starts 1 and 2;
- servo closed routes to 1, partially open routes to 2, and fully open routes to 3;
- the line-tracer network has three starts, three intersections, and three destinations, matching the reference geometry;
- sorting ultrasonic sensors are placed outside the rails at the three starts, with unobstructed live centimeter labels;
- fixed decorative rectangles inside both conveyors are omitted.

## Visual State Mapping

The drawing itself communicates node state. Text cards remain the precise accessible fallback.

State precedence, from highest to lowest, is:

1. disconnected or heartbeat-stale;
2. emergency stop or fresh error;
3. active work;
4. online/running;
5. stopped, idle, ready, or waiting.

Presentation:

| State | Drawing treatment |
| --- | --- |
| Disconnected or stale | Neutral color at about 15% opacity; motion stops |
| Emergency stop | Red, slow pulse; motion stops |
| Error | Solid red; affected motion stops |
| Active work | Blue with a restrained pulse; relevant mechanical motion enabled |
| Online/running | Green, full opacity |
| Stopped/idle/ready/waiting | Neutral gray at about 40% opacity; motion stops |

Color is always paired with the nearby process label and card text, so state is not conveyed by color alone. Reduced-motion system preferences disable pulsing and interpolate directly to the final geometry.

## Product and Equipment Motion

Animations are independent per node so several work items can be represented concurrently.

### Input

When the input node is fresh, running, and has an active work ID, the work box cycles through input positions 1, 2, and 3. On a matching detection/recognition state or input sensor detection, the box is pinned at the camera position. It continues into gripper motion only when the gripper reports the same work ID or an explicit transfer state.

### Gripper

For `PICKING` or the initial transfer phase, the arm points north and the jaws close around the box. During `TRANSFERRING`, the arm rotates clockwise toward the sorting conveyor. On `PLACED` or `COMPLETED`, the box appears on the sorting conveyor and the jaws reopen. Missing or contradictory telemetry stops the animation at the last trustworthy phase rather than fabricating progress.

### Sorting

When sorting is fresh and active, its work box cycles down the conveyor before sensor detection. A detected sorting sensor pins the box at the corresponding start. The servo uses an explicit route/destination value when one is available in current state or work data. If no route is present in current telemetry, it stays at its last known angle and the card remains the authoritative status; the UI does not guess a destination.

### Line Tracer

The line-tracer icon starts at the sensor/drop point for the selected route. Fresh delivery states move it through that route's start, intersection, and destination. Completion pins it at the destination. Disconnection, stale state, stop, or error freezes motion and applies the corresponding visual state.

### Sensor Values

Ultrasonic values use the existing `SensorUnitStatus` data. Valid distances are always visible in centimeters, including while the parent motor is stopped. Unknown or stale values display `-- cm`; they are never replaced with fabricated values.

## Telemetry Limitations

The current protocol provides node state, work ID, sensor state, and sensor distance, but it does not provide continuous physical coordinates for every conveyor position. Conveyor movement between confirmed events is therefore a presentation animation, not a measured position.

The implementation must keep that limitation explicit in code and tests. Confirmed sensor and process events override inferred animation immediately. No new protocol field is introduced in this change.

## UI Polish

Polish is limited to the existing control-center surfaces touched by this layout:

- consolidate the dark palette and status colors used by the header, dashboard, video cards, detail tabs, process controls, dialogs, and tooltips;
- normalize outer margins, panel spacing, corner radii, and divider colors;
- remove redundant borders and large unused gaps;
- retain readable product-image and log areas;
- keep video cells at a usable minimum size;
- make selected, hover, disabled, pending, warning, error, and disconnected treatments consistent;
- preserve the already themed modal dialogs and align any remaining native-looking controls with them;
- keep text sizes readable and avoid black-on-black popup or dropdown content.

This is a visual consolidation, not a general refactor. Existing panel responsibilities and signal flows remain unchanged unless required for the top-view integration.

## Failure Behavior

- MQTT disconnect immediately marks every node disconnected and stops mechanical animation.
- Heartbeat expiry uses the existing stale transition and produces the same faded drawing state.
- A malformed or unknown state falls back to waiting/neutral presentation and remains visible in the card tooltip/log.
- Missing process definitions omit only the corresponding scene equipment update; the rest of the map remains operational.
- Reconnection restores drawing state from the next accepted dashboard update without restarting the application.

## Testing

Add the smallest runnable checks that protect the non-trivial presentation logic:

- state-to-color/opacity/motion mapping, including precedence;
- disconnected and stale states stopping animation;
- independent simultaneous work IDs across input, gripper, sorting, and line tracer;
- sensor detection pinning a box at the correct location;
- missing distance rendering as `-- cm` and valid distance rendering in centimeters;
- card selection highlighting the matching equipment;
- equipment selection emitting the matching device ID and selecting the card;
- gripper north-to-east rotation phases;
- sorting servo route angles when an explicit route is available;
- existing dashboard, process-control, log, and dialog tests continuing to pass.

## Acceptance Criteria

- The approved factory top-view replaces the card-only strip.
- Existing overall and node cards remain visible to the right.
- Every node's fresh, working, waiting, error, emergency-stop, and disconnected state is distinguishable in the drawing and cards.
- Input and sorting products animate independently only while their node data permits motion.
- Gripper, servo, line tracer, and ultrasonic values follow the approved geometry and behavior.
- Multiple active works can be displayed concurrently.
- Video, product image, logs, and controls remain usable at 1280x720 and 1600x900.
- No MQTT contract or backend behavior changes.
