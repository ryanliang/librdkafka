/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2020 Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include "rdkafka_int.h"
#include "rdkafka_assignor.h"


/**
 * Source: https://github.com/apache/kafka/blob/trunk/clients/src/main/java/org/apache/kafka/clients/consumer/StickyAssignor.java
 * 
 */


static rd_kafka_topic_partition_list_t *
rd_kafka_group_sticky_assignor_read_assignment (const rd_kafkap_bytes_t
						*UserData) {
	rd_kafka_topic_partition_list_t *result;

	return result;
}

rd_kafka_resp_err_t
rd_kafka_sticky_assignor_assign_cb (rd_kafka_assignor_t *rkas,
				    rd_kafka_t *rk,
				    const char *member_id,
				    const rd_kafka_metadata_t *metadata,
				    rd_kafka_group_member_t *members,
				    size_t member_cnt,
				    rd_kafka_assignor_topic_t
				    **eligible_topics,
				    size_t eligible_topic_cnt,
				    char *errstr, size_t errstr_size,
				    void *opaque) {
        unsigned int ti;
	int next = 0; /* Next member id */

	/* Sort topics by name */
	qsort(eligible_topics, eligible_topic_cnt, sizeof(*eligible_topics),
	      rd_kafka_assignor_topic_cmp);

	/* Sort members by name */
	qsort(members, member_cnt, sizeof(*members),
	      rd_kafka_group_member_cmp);

        for (ti = 0 ; ti < eligible_topic_cnt ; ti++) {
                rd_kafka_assignor_topic_t *eligible_topic = eligible_topics[ti];
		int partition;

		/* For each topic+partition, assign one member (in a cyclic
		 * iteration) per partition until the partitions are exhausted*/
		for (partition = 0 ;
		     partition < eligible_topic->metadata->partition_cnt ;
		     partition++) {
			rd_kafka_group_member_t *rkgm;

			/* Scan through members until we find one with a
			 * subscription to this topic. */
			while (!rd_kafka_group_member_find_subscription(
				       rk, &members[next],
				       eligible_topic->metadata->topic))
				next++;

			rkgm = &members[next];

			rd_kafka_dbg(rk, CGRP, "ASSIGN",
				     "sticky: Member \"%s\": "
				     "assigned topic %s partition %d",
				     rkgm->rkgm_member_id->str,
				     eligible_topic->metadata->topic,
				     partition);

			rd_kafka_topic_partition_list_add(
				rkgm->rkgm_assignment,
				eligible_topic->metadata->topic, partition);

			next = (next+1) % rd_list_cnt(&eligible_topic->members);
		}
	}

        return 0;
}


typedef struct rd_kafka_sticky_assignor_state_s {
	rd_kafka_topic_partition_list_t *prev_assignment;
	int32_t				 generation_id;
} rd_kafka_sticky_assignor_state_t;


void rd_kafka_sticky_assignor_on_assignment_cb (
		const rd_kafka_assignor_t *rkas,
		void **assignor_state,
                const rd_kafka_topic_partition_list_t *partitions,
                const rd_kafkap_bytes_t *assignment_userdata,
                const rd_kafka_consumer_group_metadata_t *rkcgm) {
	rd_kafka_sticky_assignor_state_t *state
		= (rd_kafka_sticky_assignor_state_t *)*assignor_state;

	if (!state) {
		*assignor_state = rd_malloc(
			sizeof(rd_kafka_sticky_assignor_state_t));
		state = (rd_kafka_sticky_assignor_state_t *)*assignor_state;
	} else
		rd_kafka_topic_partition_list_destroy(state->prev_assignment);

	state->prev_assignment = rd_kafka_topic_partition_list_copy(partitions);
	state->generation_id = rkcgm->generation_id;
}


rd_kafkap_bytes_t *
rd_kafka_sticky_assignor_get_metadata (rd_kafka_assignor_t *rkas,
				       void *assignor_state,
				       const rd_list_t *topics) {
	rd_kafka_sticky_assignor_state_t *state;
	rd_kafka_buf_t *rkbuf;
	rd_kafkap_bytes_t *kbytes;
	int i;
        size_t of_TopicCnt;
        const char *last_topic = NULL;
        ssize_t of_PartCnt = -1;
        int TopicCnt = 0;
        int PartCnt = 0;
	size_t len;

	/*
	 * UserData (Version: 1) => [previous_assignment] generation
	 *   previous_assignment => topic [partitions]
	 *     topic => STRING
	 *     partitions => partition
	 *       partition => INT32
	 *   generation => INT32
	 */

	/* No previous assignment */
	if (!assignor_state) {
		// java returns null, here we use empty.
		return rd_kafka_consumer_protocol_member_metadata_new(
                	topics, NULL, 0);
	}

	state = (rd_kafka_sticky_assignor_state_t *)assignor_state;

        rkbuf = rd_kafka_buf_new(1, 100);
        of_TopicCnt = rd_kafka_buf_write_i32(rkbuf, 0); /* Updated later */
        for (i = 0 ; i < state->prev_assignment->cnt ; i++) {
                const rd_kafka_topic_partition_t *rktpar;

                rktpar = &state->prev_assignment->elems[i];

                if (!last_topic || strcmp(last_topic,
                                          rktpar->topic)) {
                        if (last_topic)
                                /* Finalize previous PartitionCnt */
                                rd_kafka_buf_update_i32(rkbuf, of_PartCnt,
                                                        PartCnt);
                        rd_kafka_buf_write_str(rkbuf, rktpar->topic, -1);
                        /* Updated later */
                        of_PartCnt = rd_kafka_buf_write_i32(rkbuf, 0);
                        PartCnt = 0;
                        last_topic = rktpar->topic;
                        TopicCnt++;
                }

                rd_kafka_buf_write_i32(rkbuf, rktpar->partition);
                PartCnt++;
        }

        if (of_PartCnt != -1)
                rd_kafka_buf_update_i32(rkbuf, of_PartCnt, PartCnt);
        rd_kafka_buf_update_i32(rkbuf, of_TopicCnt, TopicCnt);

        rd_kafka_buf_write_i32(rkbuf, state->generation_id);

        /* Get binary buffer and allocate a new Kafka Bytes with a copy. */
        rd_slice_init_full(&rkbuf->rkbuf_reader, &rkbuf->rkbuf_buf);
        len = rd_slice_remains(&rkbuf->rkbuf_reader);
        kbytes = rd_kafkap_bytes_new(NULL, (int32_t)len);
        rd_slice_read(&rkbuf->rkbuf_reader, (void *)kbytes->data, len);
        rd_kafka_buf_destroy(rkbuf);

	return rd_kafka_consumer_protocol_member_metadata_new(
                topics, kbytes->data, kbytes->len);
}


void
rd_kafka_sticky_assignor_state_destroy (void *assignor_state) {
	if (assignor_state) {
		rd_kafka_sticky_assignor_state_t *state =
			(rd_kafka_sticky_assignor_state_t *)assignor_state;
		rd_kafka_topic_partition_list_destroy(state->prev_assignment);
		rd_free(state);
	}
}
