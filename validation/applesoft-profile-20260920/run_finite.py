import profile_harness as h

programs = ['RANDOM PLASMA', 'FS PLASMA', 'COLOR PLASMA', 'SIERPINSKI',
            'GUMBALLS', 'CITY SCAPE', 'SPRITE', 'ISOMETRIC FACTORY',
            'GEOMETRY', 'GEOMETRIC', 'TRUCHET TILES', 'IDENTITY CRISIS',
            'SYS.DIAG', 'HELLO']
c = h.connect()
try:
    for name in programs:
        h.capture(c, name)
finally:
    c.close()
