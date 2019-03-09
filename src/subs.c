/*
Copyright (c) 2010-2018 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License v1.0
and Eclipse Distribution License v1.0 which accompany this distribution.
 
The Eclipse Public License is available at
   http://www.eclipse.org/legal/epl-v10.html
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.
 
Contributors:
   Roger Light - initial implementation and documentation.
*/

/* A note on matching topic subscriptions.
 *
 * Topics can be up to 32767 characters in length. The / character is used as a
 * hierarchy delimiter. Messages are published to a particular topic.
 * Clients may subscribe to particular topics directly, but may also use
 * wildcards in subscriptions.  The + and # characters are used as wildcards.
 * The # wildcard can be used at the end of a subscription only, and is a
 * wildcard for the level of hierarchy at which it is placed and all subsequent
 * levels.
 * The + wildcard may be used at any point within the subscription and is a
 * wildcard for only the level of hierarchy at which it is placed.
 * Neither wildcard may be used as part of a substring.
 * Valid:
 * 	a/b/+
 * 	a/+/c
 * 	a/#
 * 	a/b/#
 * 	#
 * 	+/b/c
 * 	+/+/+
 * Invalid:
 *	a/#/c
 *	a+/b/c
 * Valid but non-matching:
 *	a/b
 *	a/+
 *	+/b
 *	b/c/a
 *	a/b/d
 */

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mosquitto_broker_internal.h"
#include "memory_mosq.h"
#include "mqtt_protocol.h"
#include "util_mosq.h"

struct sub__token {
	struct sub__token *next;
	char *topic;
	uint16_t topic_len;
};

static int subs__process(struct mosquitto_db *db, struct mosquitto__subhier *hier, const char *source_id, const char *topic, int qos, int retain, struct mosquitto_msg_store *stored, bool set_retain)
{
	int rc = 0;
	int rc2;
	int client_qos, msg_qos;
	uint16_t mid;
	struct mosquitto__subleaf *leaf;
	bool client_retain;
	mosquitto_property *properties = NULL;

	leaf = hier->subs;

	if(retain && set_retain){
#ifdef WITH_PERSISTENCE
		if(strncmp(topic, "$SYS", 4)){
			/* Retained messages count as a persistence change, but only if
			 * they aren't for $SYS. */
			db->persistence_changes++;
		}
#endif
		if(hier->retained){
			db__msg_store_deref(db, &hier->retained);
#ifdef WITH_SYS_TREE
			db->retained_count--;
#endif
		}
		if(stored->payloadlen){
			hier->retained = stored;
			hier->retained->ref_count++;
#ifdef WITH_SYS_TREE
			db->retained_count++;
#endif
		}else{
			hier->retained = NULL;
		}
	}
	while(source_id && leaf){
		if(!leaf->context->id || (leaf->no_local && !strcmp(leaf->context->id, source_id))){
			leaf = leaf->next;
			continue;
		}
		/* Check for ACL topic access. */
		rc2 = mosquitto_acl_check(db, leaf->context, topic, stored->payloadlen, UHPA_ACCESS(stored->payload, stored->payloadlen), stored->qos, stored->retain, MOSQ_ACL_READ);
		if(rc2 == MOSQ_ERR_ACL_DENIED){
			leaf = leaf->next;
			continue;
		}else if(rc2 == MOSQ_ERR_SUCCESS){
			client_qos = leaf->qos;

			if(db->config->upgrade_outgoing_qos){
				msg_qos = client_qos;
			}else{
				if(qos > client_qos){
					msg_qos = client_qos;
				}else{
					msg_qos = qos;
				}
			}
			if(msg_qos){
				mid = mosquitto__mid_generate(leaf->context);
			}else{
				mid = 0;
			}
			if(leaf->retain_as_published){
				client_retain = retain;
			}else{
				client_retain = false;
			}
			if(leaf->identifier){
				mosquitto_property_add_varint(&properties, MQTT_PROP_SUBSCRIPTION_IDENTIFIER, leaf->identifier);
			}
			if(db__message_insert(db, leaf->context, mid, mosq_md_out, msg_qos, client_retain, stored, properties) == 1) rc = 1;
		}else{
			return 1; /* Application error */
		}
		leaf = leaf->next;
	}
	if(hier->subs){
		return rc;
	}else{
		return MOSQ_ERR_NO_SUBSCRIBERS;
	}
}

static struct sub__token *sub__topic_append(struct sub__token **tail, struct sub__token **topics, char *topic)
{
	struct sub__token *new_topic;

	if(!topic){
		return NULL;
	}
	new_topic = mosquitto__malloc(sizeof(struct sub__token));
	if(!new_topic){
		return NULL;
	}
	new_topic->next = NULL;
	new_topic->topic_len = strlen(topic);
	new_topic->topic = mosquitto__malloc(new_topic->topic_len+1);
	if(!new_topic->topic){
		mosquitto__free(new_topic);
		return NULL;
	}
	strncpy(new_topic->topic, topic, new_topic->topic_len+1);

	if(*tail){
		(*tail)->next = new_topic;
		*tail = (*tail)->next;
	}else{
		*topics = new_topic;
		*tail = new_topic;
	}
	return new_topic;
}

static int sub__topic_tokenise(const char *subtopic, struct sub__token **topics)
{
	struct sub__token *new_topic, *tail = NULL;
	int len;
	int start, stop, tlen;
	int i;
	char *topic;

	assert(subtopic);
	assert(topics);

	if(subtopic[0] != '$'){
		new_topic = sub__topic_append(&tail, topics, "");
		if(!new_topic) goto cleanup;
	}

	len = strlen(subtopic);

	if(subtopic[0] == '/'){
		new_topic = sub__topic_append(&tail, topics, "");
		if(!new_topic) goto cleanup;

		start = 1;
	}else{
		start = 0;
	}

	stop = 0;
	for(i=start; i<len+1; i++){
		if(subtopic[i] == '/' || subtopic[i] == '\0'){
			stop = i;

			if(start != stop){
				tlen = stop-start;

				topic = mosquitto__malloc(tlen+1);
				if(!topic) goto cleanup;
				memcpy(topic, &subtopic[start], tlen);
				topic[tlen] = '\0';
				new_topic = sub__topic_append(&tail, topics, topic);
				mosquitto__free(topic);
			}else{
				new_topic = sub__topic_append(&tail, topics, "");
			}
			if(!new_topic) goto cleanup;
			start = i+1;
		}
	}

	return MOSQ_ERR_SUCCESS;

cleanup:
	tail = *topics;
	*topics = NULL;
	while(tail){
		mosquitto__free(tail->topic);
		new_topic = tail->next;
		mosquitto__free(tail);
		tail = new_topic;
	}
	return 1;
}

static void sub__topic_tokens_free(struct sub__token *tokens)
{
	struct sub__token *tail;

	while(tokens){
		tail = tokens->next;
		mosquitto__free(tokens->topic);
		mosquitto__free(tokens);
		tokens = tail;
	}
}

static int sub__add_recurse(struct mosquitto_db *db, struct mosquitto *context, int qos, uint32_t identifier, int options, struct mosquitto__subhier *subhier, struct sub__token *tokens)
	/* FIXME - this function has the potential to leak subhier, audit calling functions. */
{
	struct mosquitto__subhier *branch;
	struct mosquitto__subleaf *leaf, *last_leaf;
	struct mosquitto__subhier **subs;
	int i;

	if(!tokens){
		if(context && context->id){
			leaf = subhier->subs;
			last_leaf = NULL;
			while(leaf){
				if(leaf->context && leaf->context->id && !strcmp(leaf->context->id, context->id)){
					/* Client making a second subscription to same topic. Only
					 * need to update QoS. Return MOSQ_ERR_SUB_EXISTS to
					 * indicate this to the calling function. */
					leaf->qos = qos;
					leaf->identifier = identifier;
					if(context->protocol == mosq_p_mqtt31 || context->protocol == mosq_p_mqtt5){
						return MOSQ_ERR_SUB_EXISTS;
					}else{
						/* mqttv311/mqttv5 requires retained messages are resent on
						 * resubscribe. */
						return MOSQ_ERR_SUCCESS;
					}
				}
				last_leaf = leaf;
				leaf = leaf->next;
			}
			leaf = mosquitto__malloc(sizeof(struct mosquitto__subleaf));
			if(!leaf) return MOSQ_ERR_NOMEM;
			leaf->next = NULL;
			leaf->context = context;
			leaf->qos = qos;
			leaf->identifier = identifier;
			leaf->no_local = ((options & MQTT_SUB_OPT_NO_LOCAL) != 0);
			leaf->retain_as_published = ((options & MQTT_SUB_OPT_RETAIN_AS_PUBLISHED) != 0);
			for(i=0; i<context->sub_count; i++){
				if(!context->subs[i]){
					context->subs[i] = subhier;
					break;
				}
			}
			if(i == context->sub_count){
				subs = mosquitto__realloc(context->subs, sizeof(struct mosquitto__subhier *)*(context->sub_count + 1));
				if(!subs){
					mosquitto__free(leaf);
					return MOSQ_ERR_NOMEM;
				}
				context->subs = subs;
				context->sub_count++;
				context->subs[context->sub_count-1] = subhier;
			}
			if(last_leaf){
				last_leaf->next = leaf;
				leaf->prev = last_leaf;
			}else{
				subhier->subs = leaf;
				leaf->prev = NULL;
			}
#ifdef WITH_SYS_TREE
			db->subscription_count++;
#endif
		}
		return MOSQ_ERR_SUCCESS;
	}

	HASH_FIND(hh, subhier->children, tokens->topic, tokens->topic_len, branch);
	if(branch){
		return sub__add_recurse(db, context, qos, identifier, options, branch, tokens->next);
	}else{
		/* Not found */
		branch = sub__add_hier_entry(subhier, &subhier->children, tokens->topic, tokens->topic_len);
		if(!branch) return MOSQ_ERR_NOMEM;

		return sub__add_recurse(db, context, qos, identifier, options, branch, tokens->next);
	}
}

static int sub__remove_recurse(struct mosquitto_db *db, struct mosquitto *context, struct mosquitto__subhier *subhier, struct sub__token *tokens, uint8_t *reason)
{
	struct mosquitto__subhier *branch;
	struct mosquitto__subleaf *leaf;
	int i;

	if(!tokens){
		leaf = subhier->subs;
		while(leaf){
			if(leaf->context==context){
#ifdef WITH_SYS_TREE
				db->subscription_count--;
#endif
				if(leaf->prev){
					leaf->prev->next = leaf->next;
				}else{
					subhier->subs = leaf->next;
				}
				if(leaf->next){
					leaf->next->prev = leaf->prev;
				}
				mosquitto__free(leaf);

				/* Remove the reference to the sub that the client is keeping.
				 * It would be nice to be able to use the reference directly,
				 * but that would involve keeping a copy of the topic string in
				 * each subleaf. Might be worth considering though. */
				for(i=0; i<context->sub_count; i++){
					if(context->subs[i] == subhier){
						context->subs[i] = NULL;
						break;
					}
				}
				*reason = 0;
				return MOSQ_ERR_SUCCESS;
			}
			leaf = leaf->next;
		}
		return MOSQ_ERR_SUCCESS;
	}

	HASH_FIND(hh, subhier->children, tokens->topic, tokens->topic_len, branch);
	if(branch){
		sub__remove_recurse(db, context, branch, tokens->next, reason);
		if(!branch->children && !branch->subs && !branch->retained){
			HASH_DELETE(hh, subhier->children, branch);
			mosquitto__free(branch->topic);
			mosquitto__free(branch);
		}
	}
	return MOSQ_ERR_SUCCESS;
}

static int sub__search(struct mosquitto_db *db, struct mosquitto__subhier *subhier, struct sub__token *tokens, const char *source_id, const char *topic, int qos, int retain, struct mosquitto_msg_store *stored, bool set_retain)
{
	/* FIXME - need to take into account source_id if the client is a bridge */
	struct mosquitto__subhier *branch;
	int rc;
	bool have_subscribers = false;

	if(tokens){
		/* Check for literal match */
		HASH_FIND(hh, subhier->children, tokens->topic, tokens->topic_len, branch);

		if(branch){
			rc = sub__search(db, branch, tokens->next, source_id, topic, qos, retain, stored, set_retain);
			if(rc == MOSQ_ERR_SUCCESS){
				have_subscribers = true;
			}else if(rc != MOSQ_ERR_NO_SUBSCRIBERS){
				return rc;
			}
			if(!tokens->next){
				rc = subs__process(db, branch, source_id, topic, qos, retain, stored, set_retain);
				if(rc == MOSQ_ERR_SUCCESS){
					have_subscribers = true;
				}else if(rc != MOSQ_ERR_NO_SUBSCRIBERS){
					return rc;
				}
			}
		}

		/* Check for + match */
		HASH_FIND(hh, subhier->children, "+", 1, branch);

		if(branch){
			rc = sub__search(db, branch, tokens->next, source_id, topic, qos, retain, stored, false);
			if(rc == MOSQ_ERR_SUCCESS){
				have_subscribers = true;
			}else if(rc != MOSQ_ERR_NO_SUBSCRIBERS){
				return rc;
			}
			if(!tokens->next){
				rc = subs__process(db, branch, source_id, topic, qos, retain, stored, false);
				if(rc == MOSQ_ERR_SUCCESS){
					have_subscribers = true;
				}else if(rc != MOSQ_ERR_NO_SUBSCRIBERS){
					return rc;
				}
			}
		}
	}

	/* Check for # match */
	HASH_FIND(hh, subhier->children, "#", 1, branch);
	if(branch && !branch->children){
		/* The topic matches due to a # wildcard - process the
		 * subscriptions but *don't* return. Although this branch has ended
		 * there may still be other subscriptions to deal with.
		 */
		rc = subs__process(db, branch, source_id, topic, qos, retain, stored, false);
		if(rc == MOSQ_ERR_SUCCESS){
			have_subscribers = true;
		}else if(rc != MOSQ_ERR_NO_SUBSCRIBERS){
			return rc;
		}
	}

	if(have_subscribers){
		return MOSQ_ERR_SUCCESS;
	}else{
		return MOSQ_ERR_NO_SUBSCRIBERS;
	}
}


struct mosquitto__subhier *sub__add_hier_entry(struct mosquitto__subhier *parent, struct mosquitto__subhier **sibling, const char *topic, size_t len)
{
	struct mosquitto__subhier *child;

	assert(sibling);

	child = mosquitto__malloc(sizeof(struct mosquitto__subhier));
	if(!child){
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
		return NULL;
	}
	child->parent = parent;
	child->topic_len = len;
	child->topic = malloc(len+1);
	if(!child->topic){
		child->topic_len = 0;
		mosquitto__free(child);
		log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
		return NULL;
	}else{
		strncpy(child->topic, topic, child->topic_len+1);
	}
	child->subs = NULL;
	child->children = NULL;
	child->retained = NULL;

	HASH_ADD_KEYPTR(hh, *sibling, child->topic, child->topic_len, child);

	return child;
}


int sub__add(struct mosquitto_db *db, struct mosquitto *context, const char *sub, int qos, uint32_t identifier, int options, struct mosquitto__subhier **root)
{
	int rc = 0;
	struct mosquitto__subhier *subhier;
	struct sub__token *tokens = NULL;

	assert(root);
	assert(*root);
	assert(sub);

	if(sub__topic_tokenise(sub, &tokens)) return 1;

	HASH_FIND(hh, *root, tokens->topic, tokens->topic_len, subhier);
	if(!subhier){
		subhier = sub__add_hier_entry(NULL, root, tokens->topic, tokens->topic_len);
		if(!subhier){
			sub__topic_tokens_free(tokens);
			log__printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
			return MOSQ_ERR_NOMEM;
		}

	}
	rc = sub__add_recurse(db, context, qos, identifier, options, subhier, tokens);

	sub__topic_tokens_free(tokens);

	return rc;
}

int sub__remove(struct mosquitto_db *db, struct mosquitto *context, const char *sub, struct mosquitto__subhier *root, uint8_t *reason)
{
	int rc = 0;
	struct mosquitto__subhier *subhier;
	struct sub__token *tokens = NULL;

	assert(root);
	assert(sub);

	if(sub__topic_tokenise(sub, &tokens)) return 1;

	HASH_FIND(hh, root, tokens->topic, tokens->topic_len, subhier);
	if(subhier){
		*reason = MQTT_RC_NO_SUBSCRIPTION_EXISTED;
		rc = sub__remove_recurse(db, context, subhier, tokens, reason);
	}

	sub__topic_tokens_free(tokens);

	return rc;
}

int sub__messages_queue(struct mosquitto_db *db, const char *source_id, const char *topic, int qos, int retain, struct mosquitto_msg_store **stored)
{
	int rc = 0;
	struct mosquitto__subhier *subhier;
	struct sub__token *tokens = NULL;

	assert(db);
	assert(topic);

	if(sub__topic_tokenise(topic, &tokens)) return 1;

	/* Protect this message until we have sent it to all
	clients - this is required because websockets client calls
	db__message_write(), which could remove the message if ref_count==0.
	*/
	(*stored)->ref_count++;

	HASH_FIND(hh, db->subs, tokens->topic, tokens->topic_len, subhier);
	if(subhier){
		if(retain){
			/* We have a message that needs to be retained, so ensure that the subscription
			 * tree for its topic exists.
			 */
			sub__add_recurse(db, NULL, 0, 0, 0, subhier, tokens);
		}
		rc = sub__search(db, subhier, tokens, source_id, topic, qos, retain, *stored, true);
	}
	sub__topic_tokens_free(tokens);

	/* Remove our reference and free if needed. */
	db__msg_store_deref(db, stored);

	return rc;
}


/* Remove a subhier element, and return its parent if that needs freeing as well. */
static struct mosquitto__subhier *tmp_remove_subs(struct mosquitto__subhier *sub)
{
	struct mosquitto__subhier *parent;

	if(!sub || !sub->parent){
		return NULL;
	}

	if(sub->children || sub->subs || sub->retained){
		return NULL;
	}

	parent = sub->parent;
	HASH_DELETE(hh, parent->children, sub);
	mosquitto__free(sub->topic);
	mosquitto__free(sub);

	if(parent->subs == NULL
			&& parent->children == NULL
			&& parent->retained == NULL
			&& parent->parent){

		return parent;
	}else{
		return NULL;
	}
}


/* Remove all subscriptions for a client.
 */
int sub__clean_session(struct mosquitto_db *db, struct mosquitto *context)
{
	int i;
	struct mosquitto__subleaf *leaf;
	struct mosquitto__subhier *hier;

	for(i=0; i<context->sub_count; i++){
		if(context->subs[i] == NULL){
			continue;
		}
		leaf = context->subs[i]->subs;
		while(leaf){
			if(leaf->context==context){
#ifdef WITH_SYS_TREE
				db->subscription_count--;
#endif
				if(leaf->prev){
					leaf->prev->next = leaf->next;
				}else{
					context->subs[i]->subs = leaf->next;
				}
				if(leaf->next){
					leaf->next->prev = leaf->prev;
				}
				mosquitto__free(leaf);
				break;
			}
			leaf = leaf->next;
		}
		if(context->subs[i]->subs == NULL
				&& context->subs[i]->children == NULL
				&& context->subs[i]->retained == NULL
				&& context->subs[i]->parent){

			hier = context->subs[i];
			context->subs[i] = NULL;
			do{
				hier = tmp_remove_subs(hier);
			}while(hier);
		}
	}
	mosquitto__free(context->subs);
	context->subs = NULL;
	context->sub_count = 0;

	return MOSQ_ERR_SUCCESS;
}

void sub__tree_print(struct mosquitto__subhier *root, int level)
{
	int i;
	struct mosquitto__subhier *branch, *branch_tmp;
	struct mosquitto__subleaf *leaf;

	HASH_ITER(hh, root, branch, branch_tmp){
	if(level > -1){
		for(i=0; i<(level+2)*2; i++){
			printf(" ");
		}
		printf("%s", branch->topic);
		leaf = branch->subs;
		while(leaf){
			if(leaf->context){
				printf(" (%s, %d)", leaf->context->id, leaf->qos);
			}else{
				printf(" (%s, %d)", "", leaf->qos);
			}
			leaf = leaf->next;
		}
		if(branch->retained){
			printf(" (r)");
		}
		printf("\n");
	}

		sub__tree_print(branch->children, level+1);
	}
}

static int retain__process(struct mosquitto_db *db, struct mosquitto__subhier *branch, struct mosquitto *context, int sub_qos, uint32_t subscription_identifier, time_t now)
{
	int rc = 0;
	int qos;
	uint16_t mid;
	mosquitto_property *properties = NULL;
	struct mosquitto_msg_store *retained;

	if(branch->retained->message_expiry_time > 0 && now > branch->retained->message_expiry_time){
		db__msg_store_deref(db, &branch->retained);
		branch->retained = NULL;
#ifdef WITH_SYS_TREE
		db->retained_count--;
#endif
		return MOSQ_ERR_SUCCESS;
	}

	retained = branch->retained;

	rc = mosquitto_acl_check(db, context, retained->topic, retained->payloadlen, UHPA_ACCESS(retained->payload, retained->payloadlen),
			retained->qos, retained->retain, MOSQ_ACL_READ);
	if(rc == MOSQ_ERR_ACL_DENIED){
		return MOSQ_ERR_SUCCESS;
	}else if(rc != MOSQ_ERR_SUCCESS){
		return rc;
	}

	/* Check for original source access */
	if(db->config->check_retain_source && retained->source_id){
		struct mosquitto retain_ctxt;
		memset(&retain_ctxt, 0, sizeof(struct mosquitto));

		retain_ctxt.id = retained->source_id;
		retain_ctxt.username = retained->source_username;
		retain_ctxt.listener = retained->source_listener;

		rc = acl__find_acls(db, &retain_ctxt);
		if(rc) return rc;

		rc = mosquitto_acl_check(db, &retain_ctxt, retained->topic, retained->payloadlen, UHPA_ACCESS(retained->payload, retained->payloadlen),
				retained->qos, retained->retain, MOSQ_ACL_WRITE);
		if(rc == MOSQ_ERR_ACL_DENIED){
			return MOSQ_ERR_SUCCESS;
		}else if(rc != MOSQ_ERR_SUCCESS){
			return rc;
		}
	}

	if (db->config->upgrade_outgoing_qos){
		qos = sub_qos;
	} else {
		qos = retained->qos;
		if(qos > sub_qos) qos = sub_qos;
	}
	if(qos > 0){
		mid = mosquitto__mid_generate(context);
	}else{
		mid = 0;
	}
	if(subscription_identifier > 0){
		mosquitto_property_add_varint(&properties, MQTT_PROP_SUBSCRIPTION_IDENTIFIER, subscription_identifier);
	}
	return db__message_insert(db, context, mid, mosq_md_out, qos, true, retained, properties);
}

static int retain__search(struct mosquitto_db *db, struct mosquitto__subhier *subhier, struct sub__token *tokens, struct mosquitto *context, const char *sub, int sub_qos, uint32_t subscription_identifier, time_t now, int level)
{
	struct mosquitto__subhier *branch, *branch_tmp;
	int flag = 0;

	if(!strcmp(tokens->topic, "#") && !tokens->next){
		HASH_ITER(hh, subhier->children, branch, branch_tmp){
			/* Set flag to indicate that we should check for retained messages
			 * on "foo" when we are subscribing to e.g. "foo/#" and then exit
			 * this function and return to an earlier retain__search().
			 */
			flag = -1;
			if(branch->retained){
				retain__process(db, branch, context, sub_qos, subscription_identifier, now);
			}
			if(branch->children){
				retain__search(db, branch, tokens, context, sub, sub_qos, subscription_identifier, now, level+1);
			}
		}
	}else{
		if(!strcmp(tokens->topic, "+")){
			HASH_ITER(hh, subhier->children, branch, branch_tmp){
				if(tokens->next){
					if(retain__search(db, branch, tokens->next, context, sub, sub_qos, subscription_identifier, now, level+1) == -1
							|| (tokens->next && !strcmp(tokens->next->topic, "#") && level>0)){

						if(branch->retained){
							retain__process(db, branch, context, sub_qos, subscription_identifier, now);
						}
					}
				}else{
					if(branch->retained){
						retain__process(db, branch, context, sub_qos, subscription_identifier, now);
					}
				}
			}
		}else{
			HASH_FIND(hh, subhier->children, tokens->topic, tokens->topic_len, branch);
			if(branch){
				if(tokens->next){
					if(retain__search(db, branch, tokens->next, context, sub, sub_qos, subscription_identifier, now, level+1) == -1
							|| (tokens->next && !strcmp(tokens->next->topic, "#") && level>0)){

						if(branch->retained){
							retain__process(db, branch, context, sub_qos, subscription_identifier, now);
						}
					}
				}else{
					if(branch->retained){
						retain__process(db, branch, context, sub_qos, subscription_identifier, now);
					}
				}
			}
		}
	}
	return flag;
}

int sub__retain_queue(struct mosquitto_db *db, struct mosquitto *context, const char *sub, int sub_qos, uint32_t subscription_identifier)
{
	struct mosquitto__subhier *subhier;
	struct sub__token *tokens = NULL, *tail;
	time_t now;

	assert(db);
	assert(context);
	assert(sub);

	if(sub__topic_tokenise(sub, &tokens)) return 1;

	HASH_FIND(hh, db->subs, tokens->topic, tokens->topic_len, subhier);

	if(subhier){
		now = time(NULL);
		retain__search(db, subhier, tokens, context, sub, sub_qos, subscription_identifier, now, 0);
	}
	while(tokens){
		tail = tokens->next;
		mosquitto__free(tokens->topic);
		mosquitto__free(tokens);
		tokens = tail;
	}

	return MOSQ_ERR_SUCCESS;
}

