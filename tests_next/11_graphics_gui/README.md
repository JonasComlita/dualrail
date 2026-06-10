# 11 Graphics GUI

Tests mirror the six GUI layers.

Layer map:

1. framebuffer and display driver;
2. 2D primitives;
3. fonts and text;
4. compositor;
5. widgets;
6. desktop/consumer shell.

Required areas:

- golden framebuffer snapshots;
- dirty rectangles;
- text/glyph boundaries;
- z-order and occlusion;
- input focus and close events;
- widget hit testing and text editing;
- taskbar and launcher.
