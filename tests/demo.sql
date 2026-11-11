-- Schema
CREATE TABLE users (
    id INT PRIMARY KEY,
    name TEXT NOT NULL,
    age INT,
    active BOOL
);

CREATE TABLE orders (
    order_id INT PRIMARY KEY,
    user_id INT NOT NULL,
    amount FLOAT,
    item TEXT
);

SHOW TABLES;
DESCRIBE users;

-- Inserts
INSERT INTO users (id, name, age, active) VALUES
    (1, 'Alice', 30, true),
    (2, 'Bob', 25, false),
    (3, 'Carol', 35, true),
    (4, 'Dave', 28, true);

INSERT INTO orders VALUES
    (100, 1, 19.99, 'Widget'),
    (101, 1, 5.50, 'Gadget'),
    (102, 2, 100.00, 'Gizmo'),
    (103, 3, 42.25, 'Widget'),
    (104, 5, 9.99, 'Orphan Order');

SELECT * FROM users;

-- WHERE with AND / OR / comparisons
SELECT name, age FROM users WHERE age > 27 AND active = true;
SELECT name FROM users WHERE age < 26 OR name = 'Carol';

-- ORDER BY + LIMIT
SELECT name, age FROM users ORDER BY age DESC;
SELECT name, age FROM users ORDER BY age ASC LIMIT 2;

-- INNER JOIN
SELECT users.name, orders.item, orders.amount
FROM users
JOIN orders ON users.id = orders.user_id;

-- LEFT JOIN (users with no orders should still appear; order 104 has no matching user)
SELECT users.name, orders.item
FROM users
LEFT JOIN orders ON users.id = orders.user_id
ORDER BY users.name;

-- Aliases
SELECT u.name, o.amount FROM users AS u JOIN orders AS o ON u.id = o.user_id WHERE o.amount > 20;

-- UPDATE
UPDATE users SET age = age + 1 WHERE name = 'Bob';
SELECT * FROM users WHERE name = 'Bob';

-- DELETE
DELETE FROM orders WHERE amount < 10;
SELECT * FROM orders;

-- Constraint checks (expected to fail gracefully -- run separately, see demo_errors.sql)
