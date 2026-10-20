-- 演示账号：13900000000 / demo123456（仅演示，不要用于任何真实环境）
-- u_pwd_algo=1：MD5(salt + password)，与 add_p81_user.sh 一致
INSERT INTO t_user (u_tel, u_password, u_salt, u_pwd_algo, u_name, u_status)
VALUES ('13900000000',
        '8e896939365b69bca4764f807a69eaab',
        '00112233445566778899aabbccddeeff',
        1, 'demo', 0)
ON DUPLICATE KEY UPDATE
  u_password = VALUES(u_password),
  u_salt = VALUES(u_salt),
  u_pwd_algo = VALUES(u_pwd_algo),
  u_status = 0;
