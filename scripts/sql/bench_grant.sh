set -e
CNF=/etc/mysql/debian.cnf
mysql --defaults-file=$CNF -e "CREATE DATABASE IF NOT EXISTS cloud_media_bench DEFAULT CHARSET utf8mb4"
mysql --defaults-file=$CNF -e "GRANT ALL ON cloud_media_bench.* TO 'cm_test'@'localhost'"
mysql --defaults-file=$CNF -e "GRANT ALL ON cloud_media_bench.* TO 'cm_test'@'127.0.0.1'"
mysql --defaults-file=$CNF -e "FLUSH PRIVILEGES"
echo GRANT_OK
