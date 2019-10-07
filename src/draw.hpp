/*    draw.hpp
 *
 *    This file is part of the Motion application
 *    Copyright (C) 2019  Motion-Project Developers(motion-project.github.io)
 *
 *    This library is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Library General Public
 *    License as published by the Free Software Foundation; either
 *    version 2 of the License, or (at your option) any later version.
 *
 *    This library is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Library General Public License for more details.
 *
 *    You should have received a copy of the GNU Library General Public
 *    License along with this library; if not, write to the
 *    Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 *    Boston, MA  02110-1301, USA.
*/

#ifndef _INCLUDE_DRAW_H_
#define _INCLUDE_DRAW_H_

/* date/time drawing, draw.c */
    int draw_text(unsigned char *image,
              int width, int height,
              int startx, int starty,
              const char *text, int factor);
    int draw_init_chars(void);
    void draw_init_scale(struct ctx_cam *cam);

    void draw_locate_preview(struct ctx_cam *cam, struct ctx_image_data *img);
    void draw_locate(struct ctx_cam *cam, struct ctx_image_data *img);
    void draw_smartmask(struct ctx_cam *cam, unsigned char *);
    void draw_fixed_mask(struct ctx_cam *cam, unsigned char *);
    void draw_largest_label(struct ctx_cam *cam, unsigned char *);

#endif