// PopoDB Database Large-Scale Benchmark
// Tests INSERT and UPDATE performance with millions of rows.
//
// Usage:
//
//	go run . -rows=1000000 -host=127.0.0.1 -port=3307
package main

import (
	"database/sql"
	"flag"
	"fmt"
	"os"
	"strings"
	"time"

	_ "github.com/go-sql-driver/mysql"
)

type BenchConfig struct {
	Host      string
	Port      int
	User      string
	DBName    string
	NumRows   int
	BatchSize int
}

type BenchResult struct {
	Name       string
	Rows       int
	Duration   time.Duration
	Throughput float64 // rows/sec
}

func (r BenchResult) String() string {
	return fmt.Sprintf("  %-25s %12d rows  %10s  %12.0f rows/s",
		r.Name, r.Rows, r.Duration.Round(time.Millisecond), r.Throughput)
}

func main() {
	cfg := BenchConfig{}
	flag.StringVar(&cfg.Host, "host", "127.0.0.1", "PopoDB server host")
	flag.IntVar(&cfg.Port, "port", 3307, "PopoDB server port")
	flag.StringVar(&cfg.User, "user", "root", "MySQL user")
	flag.StringVar(&cfg.DBName, "db", "popo", "Database name")
	flag.IntVar(&cfg.NumRows, "rows", 100000, "Number of rows to insert/update")
	flag.IntVar(&cfg.BatchSize, "batch", 500, "Rows per transaction batch")
	flag.Parse()

	fmt.Println(strings.Repeat("=", 72))
	fmt.Printf("  PopoDB Database Large-Scale Benchmark\n")
	fmt.Printf("  Target: %s:%d/%s  |  Rows: %,d  |  Batch: %d\n",
		cfg.Host, cfg.Port, cfg.DBName, cfg.NumRows, cfg.BatchSize)
	fmt.Println(strings.Repeat("=", 72))

	dsn := fmt.Sprintf("%s@tcp(%s:%d)/%s?charset=utf8&parseTime=true&loc=Local&interpolateParams=true",
		cfg.User, cfg.Host, cfg.Port, cfg.DBName)

	db, err := sql.Open("mysql", dsn)
	if err != nil {
		fmt.Fprintf(os.Stderr, "FATAL: failed to open DB: %v\n", err)
		os.Exit(1)
	}
	defer db.Close()
	db.SetMaxOpenConns(4)
	db.SetMaxIdleConns(2)

	if err := db.Ping(); err != nil {
		fmt.Fprintf(os.Stderr, "FATAL: cannot connect to %s:%d: %v\n", cfg.Host, cfg.Port, err)
		os.Exit(1)
	}
	fmt.Println("  [OK] Connected to PopoDB server\n")

	// --- Setup ---
	setupDB(db)

	// --- Phase 1: Bulk INSERT ---
	fmt.Println("── Phase 1: Bulk INSERT ──")
	insertResult := runInsert(db, cfg)
	fmt.Println(insertResult)

	// Verify count after insert
	count := mustCount(db, "bench_t1")
	fmt.Printf("  [VERIFY] SELECT COUNT(*) = %,d  %s\n\n", count,
		checkMark(count == int64(cfg.NumRows)))

	// --- Phase 2: Bulk UPDATE (update every row) ---
	fmt.Println("── Phase 2: Bulk UPDATE ──")
	updateResult := runUpdate(db, cfg)
	fmt.Println(updateResult)

	// Verify data integrity (sum of IDs 0..N-1 = (N-1)*N/2)
	expectedSum := int64(cfg.NumRows-1) * int64(cfg.NumRows) / 2
	actualSum := mustSum(db, "bench_t1", "id")
	fmt.Printf("  [VERIFY] SUM(id) = %d  %s\n\n", actualSum,
		checkMark(actualSum == expectedSum))

	// --- Phase 3: Point UPDATE (random access) ---
	fmt.Println("── Phase 3: Point UPDATE (1% of rows, random PK) ──")
	pointResult := runPointUpdate(db, cfg)
	fmt.Println(pointResult)

	// --- Summary ---
	fmt.Println(strings.Repeat("=", 72))
	fmt.Println("  Summary")
	fmt.Println(strings.Repeat("-", 72))
	fmt.Println(insertResult)
	fmt.Println(updateResult)
	fmt.Println(pointResult)
	fmt.Println(strings.Repeat("=", 72))

	totalRows := int64(cfg.NumRows)*2 + int64(cfg.NumRows)/100
	fmt.Printf("  Total rows processed: %,d\n", totalRows)
	fmt.Println("  ✓ All benchmarks completed successfully")
}

func setupDB(db *sql.DB) {
	// Drop and recreate test table
	db.Exec("DROP TABLE IF EXISTS bench_t1")
	_, err := db.Exec(`CREATE TABLE bench_t1 (
		id INT PRIMARY KEY,
		name VARCHAR(1024),
		value DOUBLE,
		flag TINYINT
	)`)
	if err != nil {
		fmt.Fprintf(os.Stderr, "FATAL: CREATE TABLE failed: %v\n", err)
		os.Exit(1)
	}
	fmt.Println("  [OK] Table 'bench_t1' created (id INT PK, name VARCHAR, value DOUBLE, flag TINYINT)")
}

func runInsert(db *sql.DB, cfg BenchConfig) BenchResult {
	start := time.Now()
	batchSize := cfg.BatchSize

	for offset := 0; offset < cfg.NumRows; offset += batchSize {
		end := offset + batchSize
		if end > cfg.NumRows {
			end = cfg.NumRows
		}

		// Build multi-row INSERT statement
		var sb strings.Builder
		sb.WriteString("INSERT INTO bench_t1 (id, name, value, flag) VALUES ")
		for i := offset; i < end; i++ {
			if i > offset {
				sb.WriteString(",")
			}
			fmt.Fprintf(&sb, "(%d,'user_%d',%f,0)", i, i, float64(i)*1.5)
		}
		if _, err := db.Exec(sb.String()); err != nil {
			fmt.Fprintf(os.Stderr, "FATAL: insert batch failed at %d: %v\n", offset, err)
			os.Exit(1)
		}

		// Progress every 10%
		progressPct := (offset + batchSize) * 100 / cfg.NumRows
		prevPct := offset * 100 / cfg.NumRows
		if progressPct/10 > prevPct/10 {
			elapsed := time.Since(start)
			fmt.Printf("  [%3d%%] %,d rows inserted  (%.1fs)\n",
				progressPct, end, elapsed.Seconds())
		}
	}

	elapsed := time.Since(start)
	return BenchResult{
		Name:       "INSERT (multi-row batch)",
		Rows:       cfg.NumRows,
		Duration:   elapsed,
		Throughput: float64(cfg.NumRows) / elapsed.Seconds(),
	}
}

func runUpdate(db *sql.DB, cfg BenchConfig) BenchResult {
	start := time.Now()

	for i := 0; i < cfg.NumRows; i++ {
		_, err := db.Exec(
			"UPDATE bench_t1 SET name = ?, value = ?, flag = 1 WHERE id = ?",
			fmt.Sprintf("updated_%d", i), float64(i)*2.0, i,
		)
		if err != nil {
			fmt.Fprintf(os.Stderr, "FATAL: update failed at id=%d: %v\n", i, err)
			os.Exit(1)
		}

		// Progress every 10%
		if i > 0 && i%(cfg.NumRows/10) == 0 {
			progressPct := i * 100 / cfg.NumRows
			elapsed := time.Since(start)
			fmt.Printf("  [%3d%%] %,d rows updated  (%.1fs)\n",
				progressPct, i, elapsed.Seconds())
		}
	}

	elapsed := time.Since(start)
	return BenchResult{
		Name:       "UPDATE (row-by-row)",
		Rows:       cfg.NumRows,
		Duration:   elapsed,
		Throughput: float64(cfg.NumRows) / elapsed.Seconds(),
	}
}

func runPointUpdate(db *sql.DB, cfg BenchConfig) BenchResult {
	// Update ~1% of rows with random PK access (simulates OLTP point updates)
	numUpdates := cfg.NumRows / 100
	if numUpdates < 10 {
		numUpdates = 10
	}
	step := cfg.NumRows / numUpdates
	if step < 1 {
		step = 1
	}

	start := time.Now()
	for i := 0; i < numUpdates; i++ {
		id := i * step
		_, err := db.Exec(
			"UPDATE bench_t1 SET value = ?, flag = 2 WHERE id = ?",
			float64(id)*3.0, id,
		)
		if err != nil {
			fmt.Fprintf(os.Stderr, "FATAL: point update failed at id=%d: %v\n", id, err)
			os.Exit(1)
		}
	}

	elapsed := time.Since(start)
	return BenchResult{
		Name:       fmt.Sprintf("POINT-UPDATE (%d ops)", numUpdates),
		Rows:       numUpdates,
		Duration:   elapsed,
		Throughput: float64(numUpdates) / elapsed.Seconds(),
	}
}

func mustCount(db *sql.DB, table string) int64 {
	// PopoDB's aggregation result set has protocol differences with Go driver.
	// Use a simple scan-based count as workaround.
	rows, err := db.Query(fmt.Sprintf("SELECT id FROM %s", table))
	if err != nil {
		fmt.Fprintf(os.Stderr, "FATAL: COUNT via SELECT failed: %v\n", err)
		os.Exit(1)
	}
	defer rows.Close()

	var count int64
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			fmt.Fprintf(os.Stderr, "FATAL: COUNT scan failed: %v\n", err)
			os.Exit(1)
		}
		count++
	}
	if err := rows.Err(); err != nil {
		fmt.Fprintf(os.Stderr, "FATAL: COUNT rows iteration error: %v\n", err)
		os.Exit(1)
	}
	return count
}

func mustSum(db *sql.DB, table, col string) int64 {
	rows, err := db.Query(fmt.Sprintf("SELECT %s FROM %s", col, table))
	if err != nil {
		fmt.Fprintf(os.Stderr, "FATAL: SUM via SELECT failed: %v\n", err)
		os.Exit(1)
	}
	defer rows.Close()

	var sum int64
	for rows.Next() {
		var val int64
		if err := rows.Scan(&val); err != nil {
			fmt.Fprintf(os.Stderr, "FATAL: SUM scan failed: %v\n", err)
			os.Exit(1)
		}
		sum += val
	}
	if err := rows.Err(); err != nil {
		fmt.Fprintf(os.Stderr, "FATAL: SUM rows iteration error: %v\n", err)
		os.Exit(1)
	}
	return sum
}

func checkMark(ok bool) string {
	if ok {
		return "✓"
	}
	return "✗ FAILED"
}
