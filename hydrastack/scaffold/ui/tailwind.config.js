/** @type {import('tailwindcss').Config} */
export default {
  content: ["./src/**/*.{ts,tsx}"],
  theme: {
    extend: {
      colors: {
        deep: {
          900: "#0d1117",
          700: "#22272e"
        },
        accent: {
          500: "#36cfc9"
        }
      }
    }
  },
  plugins: []
};
