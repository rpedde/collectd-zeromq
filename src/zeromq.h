

typedef struct numpart {
  uint16_t  type;
  uint16_t  length;
  uint64_t  value;
} numpart_t;




typedef struct strpart {
  uint16_t  type;
  uint16_t  length;
  char      *value;
} strpart_t;


typedef struct value {
  uint8_t   data_type;
  uint64_t  value;
} value_t;

typedef struct valpart {
  uint16_t  type;
  uint16_t  length;
  uint16_t  values_count;
  value_t   *values;
} valpart_t;



typedef struct packet {
  strpart_t   host;
  numpart_t   time;
  strpart_t   plugin;
  strpart_t   plugin_instance;
  strpart_t   type;
  strpart_t   type_instance;
  numpart_t   *interval;
  strpart_t   *notitication;
  numpart_t   *severity;
} packet_t;





  