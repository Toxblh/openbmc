/* just what server_ctl.c uses of libgpiod v1: see test_server_ctl.c */
#ifndef STUB_GPIOD_H
#define STUB_GPIOD_H
struct gpiod_chip;
struct gpiod_line;
struct gpiod_chip *gpiod_chip_open(const char *path);
void gpiod_chip_close(struct gpiod_chip *chip);
struct gpiod_line *gpiod_chip_get_line(struct gpiod_chip *chip, unsigned int offset);
int gpiod_line_request_input(struct gpiod_line *line, const char *consumer);
int gpiod_line_request_output(struct gpiod_line *line, const char *consumer, int default_val);
int gpiod_line_get_value(struct gpiod_line *line);
void gpiod_line_release(struct gpiod_line *line);
#endif
