#!/usr/bin/env bash

set -euo pipefail

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly migrations_dir="${script_dir}/migrations"
readonly bootstrap_file="${migrations_dir}/000_schema_migrations.sql"

readonly db_host="${DB_HOST:-127.0.0.1}"
readonly db_port="${DB_PORT:-13306}"
readonly db_user="${DB_USER:-p2}"
readonly db_password="${DB_PASSWORD:-p2_dev}"
readonly db_name="${DB_NAME:-online_game_backend}"

mysql_client() {
    MYSQL_PWD="${db_password}" mysql --protocol=TCP --host="${db_host}" --port="${db_port}" --user="${db_user}" --database="${db_name}" --connect-timeout=5 --batch --skip-column-names "$@"
}

if ! command -v mysql >/dev/null 2>&1; then
    printf '[错误] 未找到 mysql 客户端\n' >&2
    exit 1
fi

if ! command -v sha256sum >/dev/null 2>&1; then
    printf '[错误] 未找到 sha256sum 命令\n' >&2
    exit 1
fi

if [[ ! -f "${bootstrap_file}" ]]; then
    printf '[错误] 未找到迁移记录表初始化文件：%s\n' "${bootstrap_file}" >&2
    exit 1
fi

printf '[初始化] 000_schema_migrations.sql\n'
mysql_client < "${bootstrap_file}"

shopt -s nullglob
migration_files=("${migrations_dir}"/[0-9][0-9][0-9]_*.sql)

for migration_file in "${migration_files[@]}"; do
    migration_name="$(basename -- "${migration_file}")"

    if [[ ! "${migration_name}" =~ ^[0-9]{3}_[a-z0-9_]+\.sql$ ]]; then
        printf '[错误] 迁移文件名不合法：%s\n' "${migration_name}" >&2
        exit 1
    fi

    version_text="${migration_name%%_*}"
    version=$((10#${version_text}))

    if ((version == 0)); then
        continue
    fi

    checksum_line="$(sha256sum "${migration_file}")"
    checksum="${checksum_line%% *}"

    if [[ ! "${checksum}" =~ ^[0-9a-f]{64}$ ]]; then
        printf '[错误] 校验和不合法：%s\n' "${migration_name}" >&2
        exit 1
    fi

    applied_count="$(mysql_client --execute="SELECT COUNT(*) FROM schema_migrations WHERE version = ${version};")"

    if [[ "${applied_count}" == "1" ]]; then
        applied_name="$(mysql_client --execute="SELECT name FROM schema_migrations WHERE version = ${version};")"
        applied_checksum="$(mysql_client --execute="SELECT checksum FROM schema_migrations WHERE version = ${version};")"

        if [[ "${applied_name}" == "${migration_name}" && "${applied_checksum}" == "${checksum}" ]]; then
            printf '[跳过] %s\n' "${migration_name}"
            continue
        fi

        printf '[错误] 检测到迁移文件漂移，版本 %s\n' "${version_text}" >&2
        printf '数据库记录：%s %s\n' "${applied_name}" "${applied_checksum}" >&2
        printf '当前文件：%s %s\n' "${migration_name}" "${checksum}" >&2
        exit 1
    fi

    if [[ "${applied_count}" != "0" ]]; then
        printf '[错误] 迁移记录数量异常，版本 %s\n' "${version_text}" >&2
        exit 1
    fi

    printf '[执行] %s\n' "${migration_name}"

    if ! mysql_client < "${migration_file}"; then
        printf '[错误] 迁移执行失败，未写入版本记录\n' >&2
        printf '[错误] MySQL DDL 可能已部分提交，请检查数据库后再重试\n' >&2
        exit 1
    fi

    if ! mysql_client --execute="INSERT INTO schema_migrations (version, name, checksum) VALUES (${version}, '${migration_name}', '${checksum}');"; then
        printf '[错误] SQL 已执行，但版本记录写入失败\n' >&2
        printf '[错误] 请检查数据库后再重试\n' >&2
        exit 1
    fi

    printf '[完成] %s\n' "${migration_name}"
done

printf '[成功] 数据库迁移已是最新状态\n'
