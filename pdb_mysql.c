/*
 * MySQL password backend for samba
 * Copyright (C) Jelmer Vernooij 2002-2004
 * Copyright (C) Wilco Baan Hofman 2006
 * 
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 * 
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 675
 * Mass Ave, Cambridge, MA 02139, USA.
 *
 * TODO
 * * Volker commited Trust domain passwords to be included in the pdb.
 *   These need to be added here:
 *   BOOL get_trusteddom_pw(struct pdb_methods *methods, const char *domain, char **pwd, DOM_SID *sid, time_t *pass_last_set_time)
 *   BOOL set_trusteddom_pw(struct pdb_methods *methods, const char *domain, const char *pwd, const DOM_SID *sid)
 *   BOOL del_trusteddom_pw(struct pdb_methods *methods, const char *domain)
 *   NTSTATUS enum_trusteddoms(struct pdb_methods *methods, TALLOC_CTX *mem_ctx, uint32 *num_domains, struct trustdom_info ***domains)
 */

#include "pdb_sql.h"
#include <mysql.h>
#include <errmsg.h>

#define CONFIG_HOST_DEFAULT				"localhost"
#define CONFIG_USER_DEFAULT				"samba"
#define CONFIG_PASS_DEFAULT				""
#define CONFIG_PORT_DEFAULT				"3306"
#define CONFIG_DB_DEFAULT				"samba"

static int mysqlsam_debug_level = DBGC_ALL;

#undef DBGC_CLASS
#define DBGC_CLASS mysqlsam_debug_level

typedef struct pdb_mysql_data {
	MYSQL *handle;
	MYSQL_RES *pwent;
	const char *location;
} pdb_mysql_data;

#define SET_DATA(data,methods) { \
	if(!methods){ \
		DEBUG(0, ("invalid methods!\n")); \
			return NT_STATUS_INVALID_PARAMETER; \
	} \
	data = (struct pdb_mysql_data *)methods->private_data; \
		if(!data || !(data->handle)){ \
			DEBUG(0, ("invalid handle!\n")); \
				return NT_STATUS_INVALID_HANDLE; \
		} \
}

#define config_value( data, name, default_value ) \
  lp_parm_const_string( GLOBAL_SECTION_SNUM, (data)->location, name, default_value )

static long xatol(const char *d)
{
	if(!d) return 0;
	return atol(d);
}

static NTSTATUS pdb_mysql_connect(struct pdb_mysql_data *data) {
	/* Connect to mysql database */
	if (!mysql_real_connect(data->handle,
			config_value(data, "mysql host", CONFIG_HOST_DEFAULT),
			config_value(data, "mysql user", CONFIG_USER_DEFAULT),
			config_value(data, "mysql password", CONFIG_PASS_DEFAULT),
			config_value(data, "mysql database", CONFIG_DB_DEFAULT),
			xatol(config_value (data, "mysql port", CONFIG_PORT_DEFAULT)), 
			NULL, 0)) {
		DEBUG(0,
			  ("Failed to connect to mysql database: error: %s\n",
			   mysql_error(data->handle)));
		return NT_STATUS_UNSUCCESSFUL;
	}
	
	DEBUG(5, ("Connected to mysql db\n"));
	
	return NT_STATUS_OK;
}

static NTSTATUS row_to_sam_account(MYSQL_RES * r, struct samu * u)
{
	MYSQL_ROW row;
	unsigned char pwd[16];
	unsigned int num_fields;
	DOM_SID sid;

	num_fields = mysql_num_fields(r);
	row = mysql_fetch_row(r);
	if (!row) {
		DEBUG(10, ("empty result"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	pdb_set_logon_time(u, xatol(row[0]), PDB_SET);
	pdb_set_logoff_time(u, xatol(row[1]), PDB_SET);
	pdb_set_kickoff_time(u, xatol(row[2]), PDB_SET);
	pdb_set_pass_last_set_time(u, xatol(row[3]), PDB_SET);
	pdb_set_pass_can_change_time(u, xatol(row[4]), PDB_SET);
	pdb_set_pass_must_change_time(u, xatol(row[5]), PDB_SET);
	pdb_set_username(u, row[6], PDB_SET);
	pdb_set_domain(u, row[7], PDB_SET);
	pdb_set_nt_username(u, row[8], PDB_SET);
	pdb_set_fullname(u, row[9], PDB_SET);
	pdb_set_homedir(u, row[10], PDB_SET);
	pdb_set_dir_drive(u, row[11], PDB_SET);
	pdb_set_logon_script(u, row[12], PDB_SET);
	pdb_set_profile_path(u, row[13], PDB_SET);
	pdb_set_acct_desc(u, row[14], PDB_SET);
	pdb_set_workstations(u, row[15], PDB_SET);
	pdb_set_comment(u, row[16], PDB_SET);
	pdb_set_munged_dial(u, row[17], PDB_SET);

	if(!row[18] || !string_to_sid(&sid, row[18])) {
		DEBUG(0,("No user SID retrieved from database!\n"));
	} else {
		pdb_set_user_sid(u, &sid, PDB_SET);
	}

	if(row[19]) {
		string_to_sid(&sid, row[19]);
		pdb_set_group_sid(u, &sid, PDB_SET);
	}

	if (pdb_gethexpwd(row[20], pwd))
		pdb_set_lanman_passwd(u, pwd, PDB_SET);
	if (pdb_gethexpwd(row[21], pwd))
		pdb_set_nt_passwd(u, pwd, PDB_SET);

	/* Only use plaintext password storage when lanman and nt are
	 * NOT used */
	if (!row[20] || !row[21])
		pdb_set_plaintext_passwd(u, row[22]);

	pdb_set_acct_ctrl(u, xatol(row[23]), PDB_SET);
	pdb_set_logon_divs(u, xatol(row[24]), PDB_SET);
	pdb_set_hours_len(u, xatol(row[25]), PDB_SET);
	pdb_set_bad_password_count(u, xatol(row[26]), PDB_SET);
	pdb_set_logon_count(u, xatol(row[27]), PDB_SET);
	pdb_set_unknown_6(u, xatol(row[28]), PDB_SET);
	pdb_set_hours(u, (uint8 *)row[29], PDB_SET);
	
	if (row[30]) {
		uint8 pwhist[MAX_PW_HISTORY_LEN * PW_HISTORY_ENTRY_LEN];
		int i;
		
		memset(&pwhist, 0, MAX_PW_HISTORY_LEN * PW_HISTORY_ENTRY_LEN);
		for (i = 0; i < MAX_PW_HISTORY_LEN && i < strlen(row[30])/64; i++) {
			pdb_gethexpwd(&(row[30])[i*64], &pwhist[i*PW_HISTORY_ENTRY_LEN]);
			pdb_gethexpwd(&(row[30])[i*64+32], 
					&pwhist[i*PW_HISTORY_ENTRY_LEN+PW_HISTORY_SALT_LEN]);
		}
		pdb_set_pw_history(u, pwhist, strlen(row[30])/64, PDB_SET);
	}

	return NT_STATUS_OK;
}

static NTSTATUS mysqlsam_setsampwent(struct pdb_methods *methods, BOOL update, uint32 acb_mask)
{
	struct pdb_mysql_data *data =
		(struct pdb_mysql_data *) methods->private_data;
	char *query;
	int mysql_ret;

	if (!data || !(data->handle)) {
		DEBUG(0, ("invalid handle!\n"));
		return NT_STATUS_INVALID_HANDLE;
	}

	query = sql_account_query_select(NULL, data->location, update, SQL_SEARCH_NONE, NULL);

	mysql_ret = mysql_query(data->handle, query);
	
	/* [SYN] If the server has gone away, reconnect and retry */
	if (mysql_ret && mysql_errno(data->handle) == CR_SERVER_GONE_ERROR) {
		DEBUG(5, ("MySQL server has gone away, reconnecting and retrying.\n"));

		/* [SYN] Reconnect */
		if (!NT_STATUS_IS_OK(pdb_mysql_connect(data))) {
			DEBUG(0, ("Error: Lost connection to MySQL server\n"));
			talloc_free(query);
			return NT_STATUS_UNSUCCESSFUL;
		}
		/* [SYN] Retry */
		mysql_ret = mysql_query(data->handle, query);
	}
	
	talloc_free(query);

	if (mysql_ret) {
		DEBUG(0,
			   ("Error executing MySQL query %s\n", mysql_error(data->handle)));
		return NT_STATUS_UNSUCCESSFUL;
	}

	data->pwent = mysql_store_result(data->handle);

	if (data->pwent == NULL) {
		DEBUG(0,
			("Error storing results: %s\n", mysql_error(data->handle)));
		return NT_STATUS_UNSUCCESSFUL;
	}
	
	DEBUG(5,
		("mysqlsam_setsampwent succeeded(%llu results)!\n",
				mysql_num_rows(data->pwent)));
	
	return NT_STATUS_OK;
}

/***************************************************************
  End enumeration of the passwd list.
 ****************************************************************/

static void mysqlsam_endsampwent(struct pdb_methods *methods)
{
	struct pdb_mysql_data *data =
		(struct pdb_mysql_data *) methods->private_data;

	if (data == NULL) {
		DEBUG(0, ("invalid handle!\n"));
		return;
	}

	if (data->pwent != NULL)
		mysql_free_result(data->pwent);

	data->pwent = NULL;

	DEBUG(5, ("mysql_endsampwent called\n"));
}

/*****************************************************************
  Get one struct samu from the list (next in line)
 *****************************************************************/

static NTSTATUS mysqlsam_getsampwent(struct pdb_methods *methods, struct samu * user)
{
	struct pdb_mysql_data *data;

	SET_DATA(data, methods);

	if (data->pwent == NULL) {
		DEBUG(0, ("invalid pwent\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	return row_to_sam_account(data->pwent, user);
}

static NTSTATUS mysqlsam_select_by_field(struct pdb_methods * methods, struct samu * user,
						 enum sql_search_field field, const char *sname)
{
	char *esc_sname;
	char *query;
	NTSTATUS ret;
	MYSQL_RES *res;
	int mysql_ret;
	struct pdb_mysql_data *data;
	char *tmp_sname;
	TALLOC_CTX *mem_ctx = talloc_init("mysqlsam_select_by_field");

	SET_DATA(data, methods);

	esc_sname = talloc_array(mem_ctx, char, strlen(sname) * 2 + 1);
	if (!esc_sname) {
		talloc_free(mem_ctx);
		return NT_STATUS_NO_MEMORY; 
	}

	tmp_sname = talloc_strdup(mem_ctx, sname);
	
	/* Escape sname */
	mysql_real_escape_string(data->handle, esc_sname, tmp_sname,
							 strlen(tmp_sname));

	talloc_free(tmp_sname);

	if (user == NULL) {
		DEBUG(0, ("pdb_getsampwnam: struct samu is NULL.\n"));
		talloc_free(mem_ctx);
		return NT_STATUS_INVALID_PARAMETER;
	}

	query = sql_account_query_select(mem_ctx, data->location, True, field, esc_sname);

	talloc_free(esc_sname);

	DEBUG(5, ("Executing query %s\n", query));
	
	mysql_ret = mysql_query(data->handle, query);
	
	/* [SYN] If the server has gone away, reconnect and retry */
	if (mysql_ret && mysql_errno(data->handle) == CR_SERVER_GONE_ERROR) {
		DEBUG(5, ("MySQL server has gone away, reconnecting and retrying.\n"));

		/* [SYN] Reconnect */
		if (!NT_STATUS_IS_OK(pdb_mysql_connect(data))) {
			DEBUG(0, ("Error: Lost connection to MySQL server\n"));
			talloc_free(query);
			return NT_STATUS_UNSUCCESSFUL;
		}
		/* [SYN] Retry */
		mysql_ret = mysql_query(data->handle, query);
	}
	
	talloc_free(query);
	
	if (mysql_ret) {
		DEBUG(0,
			("Error while executing MySQL query %s\n", 
				mysql_error(data->handle)));
		talloc_free(mem_ctx);
		return NT_STATUS_UNSUCCESSFUL;
	}
	
	res = mysql_store_result(data->handle);
	if (res == NULL) {
		DEBUG(0,
			("Error storing results: %s\n", mysql_error(data->handle)));
		talloc_free(mem_ctx);
		return NT_STATUS_UNSUCCESSFUL;
	}
	
	ret = row_to_sam_account(res, user);
	mysql_free_result(res);
	talloc_free(mem_ctx);

	return ret;
}

/******************************************************************
  Lookup a name in the SAM database
 ******************************************************************/

static NTSTATUS mysqlsam_getsampwnam(struct pdb_methods *methods, struct samu * user,
					 const char *sname)
{
	struct pdb_mysql_data *data;

	SET_DATA(data, methods);

	if (!sname) {
		DEBUG(0, ("invalid name specified"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	return mysqlsam_select_by_field(methods, user,
			SQL_SEARCH_USER_NAME, sname);
}


/***************************************************************************
  Search by sid
 **************************************************************************/

static NTSTATUS mysqlsam_getsampwsid(struct pdb_methods *methods, struct samu * user,
					 const DOM_SID * sid)
{
	struct pdb_mysql_data *data;
	fstring sid_str;

	SET_DATA(data, methods);

	sid_to_string(sid_str, sid);

	return mysqlsam_select_by_field(methods, user, SQL_SEARCH_USER_SID, sid_str);
}

/***************************************************************************
  Delete a sam account 
 ****************************************************************************/

static NTSTATUS mysqlsam_delete_sam_account(struct pdb_methods *methods,
							struct samu * sam_pass)
{
	const char *sname = pdb_get_username(sam_pass);
	char *esc;
	char *query;
	int mysql_ret;
	struct pdb_mysql_data *data;
	char *tmp_sname;
	TALLOC_CTX *mem_ctx;
	SET_DATA(data, methods);

	if (!methods) {
		DEBUG(0, ("invalid methods!\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	data = (struct pdb_mysql_data *) methods->private_data;
	if (!data || !(data->handle)) {
		DEBUG(0, ("invalid handle!\n"));
		return NT_STATUS_INVALID_HANDLE;
	}

	if (!sname) {
		DEBUG(0, ("invalid name specified\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	mem_ctx = talloc_init("mysqlsam_delete_sam_account");
	
	/* Escape sname */
	esc = talloc_array(mem_ctx, char, strlen(sname) * 2 + 1);
	if (!esc) {
		DEBUG(0, ("Can't allocate memory to store escaped name\n"));
		return NT_STATUS_NO_MEMORY;
	}
	
	tmp_sname = talloc_strdup(mem_ctx, sname);
	
	mysql_real_escape_string(data->handle, esc, tmp_sname,
							 strlen(tmp_sname));

	talloc_free(tmp_sname);

	query = sql_account_query_delete(mem_ctx, data->location, esc);

	talloc_free(esc);

	mysql_ret = mysql_query(data->handle, query);
	
	/* [SYN] If the server has gone away, reconnect and retry */
	if (mysql_ret && mysql_errno(data->handle) == CR_SERVER_GONE_ERROR) {
		DEBUG(5, ("MySQL server has gone away, reconnecting and retrying.\n"));

		/* [SYN] Reconnect */
		if (!NT_STATUS_IS_OK(pdb_mysql_connect(data))) {
			DEBUG(0, ("Error: Lost connection to MySQL server\n"));
			talloc_free(query);
			return NT_STATUS_UNSUCCESSFUL;
		}
		/* [SYN] Retry */
		mysql_ret = mysql_query(data->handle, query);
	}

	talloc_free(query);

	if (mysql_ret) {
		DEBUG(0,
			  ("Error while executing query: %s\n",
			   mysql_error(data->handle)));
		talloc_free(mem_ctx);
		return NT_STATUS_UNSUCCESSFUL;
	}

	DEBUG(5, ("User '%s' deleted\n", sname));
	talloc_free(mem_ctx);
	return NT_STATUS_OK;
}

static NTSTATUS mysqlsam_replace_sam_account(struct pdb_methods *methods,
							 struct samu * newpwd, char isupdate)
{
	struct pdb_mysql_data *data;
	char *query;
	int mysql_ret;

	if (!methods) {
		DEBUG(0, ("invalid methods!\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	data = (struct pdb_mysql_data *) methods->private_data;

	if (data == NULL || data->handle == NULL) {
		DEBUG(0, ("invalid handle!\n"));
		return NT_STATUS_INVALID_HANDLE;
	}

	query = sql_account_query_update(NULL, data->location, newpwd, isupdate);
 	if ( query == NULL ) /* Nothing to update. */
 		return NT_STATUS_OK;
	
	/* Execute the query */
	mysql_ret = mysql_query(data->handle, query);
	
	/* [SYN] If the server has gone away, reconnect and retry */
	if (mysql_ret && mysql_errno(data->handle) == CR_SERVER_GONE_ERROR) {
		DEBUG(5, ("MySQL server has gone away, reconnecting and retrying.\n"));

		/* [SYN] Reconnect */
		if (!NT_STATUS_IS_OK(pdb_mysql_connect(data))) {
			DEBUG(0, ("Error: Lost connection to MySQL server\n"));
			talloc_free(query);
			return NT_STATUS_UNSUCCESSFUL;
		}
		/* [SYN] Retry */
		mysql_ret = mysql_query(data->handle, query);
	}
	
	if (mysql_ret) {
		DEBUG(0,
			  ("Error executing %s, %s\n", query,
			   mysql_error(data->handle)));
		talloc_free(query);
		return NT_STATUS_INVALID_PARAMETER;
	}

	talloc_free(query);

	return NT_STATUS_OK;
}

static NTSTATUS mysqlsam_add_sam_account(struct pdb_methods *methods, struct samu * newpwd)
{
	return mysqlsam_replace_sam_account(methods, newpwd, 0);
}

static NTSTATUS mysqlsam_update_sam_account(struct pdb_methods *methods,
							struct samu * newpwd)
{
	return mysqlsam_replace_sam_account(methods, newpwd, 1);
}

static BOOL mysqlsam_rid_algorithm (struct pdb_methods *pdb_methods) {
	return True;
}
static BOOL mysqlsam_new_rid (struct pdb_methods *pdb_methods, uint32 *rid) {
	return False;
}

static NTSTATUS mysqlsam_init(struct pdb_methods **pdb_method, const char *location)
{
	NTSTATUS nt_status;
	struct pdb_mysql_data *data;

	mysqlsam_debug_level = debug_add_class("mysqlsam");
	if (mysqlsam_debug_level == -1) {
		mysqlsam_debug_level = DBGC_ALL;
		DEBUG(0,
			  ("mysqlsam: Couldn't register custom debugging class!\n"));
	}

        if ( !NT_STATUS_IS_OK(nt_status = make_pdb_method( pdb_method )) ) {
		return nt_status;
        }
	
	(*pdb_method)->name = "mysqlsam";

	(*pdb_method)->setsampwent = mysqlsam_setsampwent;
	(*pdb_method)->endsampwent = mysqlsam_endsampwent;
	(*pdb_method)->getsampwent = mysqlsam_getsampwent;
	(*pdb_method)->getsampwnam = mysqlsam_getsampwnam;
	(*pdb_method)->getsampwsid = mysqlsam_getsampwsid;
	(*pdb_method)->add_sam_account = mysqlsam_add_sam_account;
	(*pdb_method)->update_sam_account = mysqlsam_update_sam_account;
	(*pdb_method)->delete_sam_account = mysqlsam_delete_sam_account;

/*	(*pdb_method)->rename_sam_account = mysqlsam_rename_sam_account; */
/*	(*pdb_method)->getgrsid = mysqlsam_getgrsid; */
/*	(*pdb_method)->getgrgid = mysqlsam_getgrgid; */
/*	(*pdb_method)->getgrnam = mysqlsam_getgrnam; */
/*	(*pdb_method)->add_group_mapping_entry = mysqlsam_add_group_mapping_entry; */
/*	(*pdb_method)->update_group_mapping_entry = mysqlsam_update_group_mapping_entry; */
/*	(*pdb_method)->delete_group_mapping_entry = mysqlsam_delete_group_mapping_entry; */
/*	(*pdb_method)->enum_group_mapping = mysqlsam_enum_group_mapping; */
/*	(*pdb_method)->get_account_policy = mysqlsam_get_account_policy; */
/*	(*pdb_method)->set_account_policy = mysqlsam_set_account_policy; */
/*	(*pdb_method)->get_seq_num = mysqlsam_get_seq_num; */

	(*pdb_method)->rid_algorithm = mysqlsam_rid_algorithm; 
	(*pdb_method)->new_rid = mysqlsam_new_rid;

	data = talloc(*pdb_method, struct pdb_mysql_data);
	(*pdb_method)->private_data = data;
	data->handle = NULL;
	data->pwent = NULL;

	if (!location) {
		DEBUG(0, ("No identifier specified. Check the Samba HOWTO Collection for details\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	data->location = smb_xstrdup(location);

	DEBUG(1,
		  ("Connecting to database server, host: %s, user: %s, database: %s, port: %ld\n",
		   config_value(data, "mysql host", CONFIG_HOST_DEFAULT),
		   config_value(data, "mysql user", CONFIG_USER_DEFAULT),
		   config_value(data, "mysql database", CONFIG_DB_DEFAULT),
		   xatol(config_value(data, "mysql port", CONFIG_PORT_DEFAULT))));

	/* Do the mysql initialization */
	data->handle = mysql_init(NULL);
	if (!data->handle) {
		DEBUG(0, ("Failed to connect to server\n"));
		return NT_STATUS_UNSUCCESSFUL;
	}

	if(!sql_account_config_valid(data->location)) {
		return NT_STATUS_INVALID_PARAMETER;
	}
	
	if (!NT_STATUS_IS_OK(pdb_mysql_connect(data))) {
		return NT_STATUS_UNSUCCESSFUL;
	}

	return NT_STATUS_OK;
}

NTSTATUS init_module(void) 
{
	return smb_register_passdb(PASSDB_INTERFACE_VERSION, "mysql", mysqlsam_init);
}

