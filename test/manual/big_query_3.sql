WITH return_analysis AS (
  SELECT
    o.order_id,
    o.user_id,
    o.product_id,
    p.category,
    u.region,
    pm.amount as payment_amount,
    r.return_id,
    r.reason as return_reason,
    CASE WHEN r.return_id IS NOT NULL THEN 1 ELSE 0 END as is_returned,
    EXTRACT(EPOCH FROM (r.return_date - o.order_date)) / 86400 as days_to_return
  FROM orders o
  JOIN products p ON o.product_id = p.product_id
  JOIN users u ON o.user_id = u.user_id
  JOIN payments pm ON o.order_id = pm.order_id
  LEFT JOIN returns r ON o.order_id = r.order_id
),
profitability_metrics AS (
  SELECT
    p.product_id,
    p.product_name,
    p.category,
    p.price,
    COUNT(DISTINCT o.order_id) as total_sales_count,
    SUM(o.quantity) as units_sold,
    SUM(o.quantity * p.price) as gross_revenue,
    AVG(COALESCE(pr.discount_pct, 0)) as avg_discount_rate,
    COUNT(ret.return_id) as return_count,
    SUM(CASE WHEN ret.return_id IS NOT NULL THEN pm.amount ELSE 0 END) as total_refunds,
    SUM(o.quantity * p.price) - SUM(CASE WHEN ret.return_id IS NOT NULL THEN pm.amount ELSE 0 END) as net_revenue,
    AVG(rev.rating) as product_rating
  FROM products p
  LEFT JOIN orders o ON p.product_id = o.product_id
  LEFT JOIN payments pm ON o.order_id = pm.order_id
  LEFT JOIN returns ret ON o.order_id = ret.order_id
  LEFT JOIN promotions pr ON p.product_id = pr.product_id
    AND o.order_date BETWEEN pr.start_date AND pr.end_date
  LEFT JOIN reviews rev ON p.product_id = rev.product_id
  GROUP BY p.product_id, p.product_name, p.category, p.price
),
regional_comparison AS (
  SELECT
    u.region,
    p.category,
    COUNT(DISTINCT o.user_id) as unique_buyers,
    COUNT(DISTINCT o.order_id) as total_orders,
    SUM(pm.amount) as total_spent,
    AVG(pm.amount) as avg_transaction,
    MAX(pm.amount) as max_transaction,
    MIN(pm.amount) as min_transaction,
    PERCENTILE_CONT(0.75) WITHIN GROUP (ORDER BY pm.amount) as p75_transaction,
    PERCENTILE_CONT(0.25) WITHIN GROUP (ORDER BY pm.amount) as p25_transaction
  FROM orders o
  JOIN users u ON o.user_id = u.user_id
  JOIN products p ON o.product_id = p.product_id
  JOIN payments pm ON o.order_id = pm.order_id
  GROUP BY u.region, p.category
),
shipping_efficiency AS (
  SELECT
    w.warehouse_id,
    w.warehouse_name,
    w.region as warehouse_region,
    COUNT(s.shipping_id) as total_shipments,
    AVG(EXTRACT(EPOCH FROM (s.ship_date - o.order_date))) as avg_processing_time,
    MIN(EXTRACT(EPOCH FROM (s.ship_date - o.order_date))) as min_processing_time,
    MAX(EXTRACT(EPOCH FROM (s.ship_date - o.order_date))) as max_processing_time,
    STDDEV(EXTRACT(EPOCH FROM (s.ship_date - o.order_date))) as processing_time_stddev
  FROM warehouses w
  JOIN shipping s ON w.warehouse_id = s.warehouse_id
  JOIN orders o ON s.order_id = o.order_id
  GROUP BY w.warehouse_id, w.warehouse_name, w.region
)
SELECT
  pm.product_name,
  pm.category,
  pm.price,
  pm.total_sales_count,
  pm.units_sold,
  pm.gross_revenue,
  pm.avg_discount_rate,
  pm.return_count,
  pm.total_refunds,
  pm.net_revenue,
  pm.product_rating,
  ra.region,
  COUNT(DISTINCT ra.order_id) as region_order_count,
  AVG(ra.payment_amount) as region_avg_payment,
  SUM(ra.is_returned) as region_returns,
  AVG(ra.days_to_return) as avg_days_to_return,
  rc.unique_buyers,
  rc.total_orders as region_total_orders,
  rc.total_spent as region_total_spent,
  rc.avg_transaction,
  rc.p75_transaction,
  rc.p25_transaction,
  se.warehouse_name,
  se.warehouse_region,
  se.total_shipments,
  se.avg_processing_time,
  se.processing_time_stddev,
  RANK() OVER (PARTITION BY pm.category ORDER BY pm.net_revenue DESC) as profit_rank,
  ROW_NUMBER() OVER (PARTITION BY ra.region ORDER BY pm.gross_revenue DESC) as regional_rank,
  CUME_DIST() OVER (ORDER BY pm.product_rating) as rating_cumulative_dist,
  LAG(pm.net_revenue, 1) OVER (PARTITION BY pm.category ORDER BY pm.net_revenue DESC) as next_product_revenue,
  LEAD(pm.net_revenue, 1) OVER (PARTITION BY pm.category ORDER BY pm.net_revenue DESC) as prev_product_revenue,
  SUM(pm.net_revenue) OVER (PARTITION BY pm.category) as category_total_revenue,
  pm.net_revenue / NULLIF(SUM(pm.net_revenue) OVER (PARTITION BY pm.category), 0) as revenue_share_in_category
FROM profitability_metrics pm
JOIN return_analysis ra ON pm.product_id = ra.product_id
JOIN regional_comparison rc ON ra.region = rc.region AND pm.category = rc.category
CROSS JOIN shipping_efficiency se
WHERE pm.units_sold > 10
  AND pm.net_revenue > 0
  AND rc.total_orders > 100
  AND se.total_shipments > 500
GROUP BY pm.product_id, pm.product_name, pm.category, pm.price,
         pm.total_sales_count, pm.units_sold, pm.gross_revenue,
         pm.avg_discount_rate, pm.return_count, pm.total_refunds,
         pm.net_revenue, pm.product_rating, ra.region,
         rc.unique_buyers, rc.total_orders, rc.total_spent,
         rc.avg_transaction, rc.p75_transaction, rc.p25_transaction,
         se.warehouse_name, se.warehouse_region, se.total_shipments,
         se.avg_processing_time, se.processing_time_stddev
ORDER BY pm.net_revenue DESC, rc.total_spent DESC
LIMIT 1000;
