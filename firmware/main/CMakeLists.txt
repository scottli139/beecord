idf_component_register(SRCS "main.c"
                            "proto/crc16.c"
                            "proto/buzz_proto.c"
                            "proto/buzz_reasm.c"
                            "audio/ogg_writer.c"
                       INCLUDE_DIRS "."
                                    "proto"
                                    "audio")
