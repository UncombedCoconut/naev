#ifndef DISTANCE_FIELD_H
#  define DISTANCE_FIELD_H

void distfield_trace( int on );

double *
make_distance_mapd( double *data, unsigned int width, unsigned int height );

double *
make_distance_mapd( double *data, unsigned int width, unsigned int height );

unsigned char *
make_distance_mapb( unsigned char *img,
                    unsigned int width, unsigned int height );
float*
make_distance_mapbf( unsigned char *img,
                    unsigned int width, unsigned int height );

#endif /* DISTANCE_FIELD_H */
